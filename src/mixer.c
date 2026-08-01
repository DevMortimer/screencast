#include "mixer.h"
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_err(const char *label, int ret)
{
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "mixer: %s: %s\n", label, buf);
}

/* Output chunk: the most the mixer emits in one go, and the size of the scratch
 * buffers.  ~200 ms absorbs ordinary jitter. */
#define MIX_FIFO_CAP_MS 200

/* A primed source that goes this long without delivering is presumed dead and
 * dropped, so it cannot hold the remaining sources hostage in the min(). */
#define MIX_STALL_TIMEOUT_US (2 * 1000000LL)

/* A timestamp jump smaller than this is buffer jitter, not lost samples.
 * Capture buffers never land exactly on their nominal times, and padding a
 * millisecond per buffer would be the very drift this is here to prevent. */
#define MIX_GAP_MIN_US (30 * 1000LL)

/*
 * How long a source may deliver nothing before the mixer starts covering for it
 * with silence.
 *
 * This is what keeps the mixed track *flowing*, and that matters far beyond the
 * mixer: the output is muxed with av_interleaved_write_frame, which holds video
 * packets until audio covers the same span.  A mixer that goes quiet does not
 * produce a quiet recording, it produces a frozen picture.  Desktop audio is
 * the source that makes this real — it only has samples when something is
 * playing, so it lags the microphone constantly.
 *
 * Above ordinary buffer jitter, well below the point a viewer would notice.
 */
#define MIX_SILENCE_GRACE_US (120 * 1000LL)

/*
 * How much a source may buffer before samples are discarded.
 *
 * A backstop, not a policy: the covering above is what keeps the mix level with
 * real time, and while it works nothing accumulates this far.  It is kept small
 * anyway because this value *is* the worst-case freeze — whatever a source is
 * allowed to buffer is how far the mixed track can fall behind, and the muxer
 * turns that into exactly that much frozen video.
 */
#define MIX_BOUND_MS 500

struct MixerCtx {
    int             active[MIX_SRC_COUNT];
    int             primed[MIX_SRC_COUNT];/* has delivered ≥1 sample */
    int64_t         last_feed_us[MIX_SRC_COUNT];
    int64_t         anchor_us[MIX_SRC_COUNT]; /* pts of the source's sample 0 */
    int64_t         written[MIX_SRC_COUNT];   /* canonical samples since anchor,
                                                 inserted silence included */
    int64_t         start_us[MIX_SRC_COUNT];  /* clock instant this source
                                                 primed; written[] is measured
                                                 against time elapsed since */
    int64_t       (*now_us)(void);         /* clock; swapped out by tests */
    SwrContext     *swr[MIX_SRC_COUNT];   /* source → canonical, lazily built */
    AVAudioFifo    *fifo[MIX_SRC_COUNT];  /* canonical stereo FLTP, bounded */
    int             fifo_cap;             /* samples per emitted chunk */
    int             bound_cap;            /* samples a source may buffer */
    float          *scratch[MIX_CHANNELS];/* per-drain read buffer */
    AVFrame        *mixed;                /* reused canonical output frame */
    MixSinkFn       sink;
    void           *user;
    int             debug;
    pthread_mutex_t lock;
};

/* Default clock.  Indirected so tests can drive elapsed time exactly rather
   than sleeping through it; nothing else ever replaces it. */
static int64_t mixer_default_now_us(void)
{
    return av_gettime_relative();
}

static float clampf(float v)
{
    /* A NaN must not survive the clamp: it fails both comparisons below and
       then spreads to every sample it is summed with, taking the whole track
       out (one capture misreading its device's format is all it takes).
       Silence is the honest output for a sample with no value.  Infinities
       are already caught by the range checks. */
    if (isnan(v)) return 0.0f;
    if (v >  1.0f) return  1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

/*
 * Append `n` samples of silence to a source's FIFO.  Called with m->lock held.
 *
 * Silence is the only honest thing to put in a hole: the audio track's PTS is a
 * running sample count, so a sample that never arrives is not a dropout, it is
 * an offset every later sample inherits.  Padding counts toward written[] so
 * that a hole filled here is not filled a second time by the gap arithmetic in
 * mixer_feed.
 *
 * Written in FIFO-sized chunks because scratch is only that long; the FIFO
 * itself grows as needed and the caller drains it straight after.
 */
static void mixer_pad_silence_locked(MixerCtx *m, MixSource src, int64_t n)
{
    if (n <= 0 || !m->fifo[src]) return;

    /* Beyond the stall timeout the source is dropped anyway — don't allocate
       an unbounded run of silence for one that is never coming back.  Truncating
       does shift this source's timeline, so it is worth saying out loud. */
    int64_t limit = MIX_SAMPLE_RATE * (MIX_STALL_TIMEOUT_US / 1000) / 1000;
    if (n > limit) {
        fprintf(stderr, "mixer: source %d reported a %lldms gap, longer than "
                        "the stall timeout — filling %lldms (it will shift)\n",
                src, (long long)(n * 1000 / MIX_SAMPLE_RATE),
                (long long)(limit * 1000 / MIX_SAMPLE_RATE));
        n = limit;
    }

    memset(m->scratch[0], 0, sizeof(float) * (size_t)m->fifo_cap);
    memset(m->scratch[1], 0, sizeof(float) * (size_t)m->fifo_cap);
    void *planes[MIX_CHANNELS] = { m->scratch[0], m->scratch[1] };

    while (n > 0) {
        int chunk = n > m->fifo_cap ? m->fifo_cap : (int)n;
        if (av_audio_fifo_write(m->fifo[src], planes, chunk) != chunk) break;
        m->written[src] += chunk;
        n -= chunk;
    }
}

/*
 * Emit min(available) samples across the active sources, summed at unity gain
 * and clamped. Called with m->lock held.
 *
 * Only *primed* sources — those that have delivered at least one sample —
 * participate in the min().  A source that is declared active at create time
 * but never actually delivers (e.g. a system-audio backend that silently fails
 * to start) would otherwise pin min() at zero forever and suppress the entire
 * mixed track.  A source that primes and then stalls is dropped outright.
 */
static void mixer_drain_locked(MixerCtx *m)
{
    int64_t now = m->now_us();

    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        if (!m->active[i] || !m->primed[i]) continue;
        if (now - m->last_feed_us[i] > MIX_STALL_TIMEOUT_US) {
            fprintf(stderr, "mixer: source %d stalled — dropping from mix\n", i);
            m->active[i] = 0;
            av_audio_fifo_drain(m->fifo[i], av_audio_fifo_size(m->fifo[i]));
        }
    }

    /*
     * Cover for a source that is not delivering *now*.
     *
     * The test is whether a source's *content* has kept up with real time, not
     * whether it has gone quiet.  That distinction is the bug this was rewritten
     * for: desktop audio never goes silent, it delivers steadily but with less
     * than a second of audio per second of recording.  min() then tracks the
     * short source, the other backs up to its bound, and the whole mixed track
     * drifts behind real time by that much.
     *
     * Audio that is merely *late* still plays back perfectly — every sample is
     * there — so nothing is audible.  The damage lands on video: the muxer holds
     * video packets until audio covers the same span, so a mix running a second
     * behind freezes the picture for a second.  Bounding this bounds that.
     *
     * The measure is elapsed time on the clock, never how deep a buffer looks.
     * That distinction is the whole correctness of this function.  Silence
     * inserted for a timestamp gap makes a buffer deeper without any time
     * having passed, so a depth-based rule reads a *healthy* source as lagging
     * and injects a gap it never had — turning one source's loss into a shift
     * of the entire mix.  Elapsed time cannot be forged that way: padding moves
     * a source's content forward, so a source that jumped is ahead here, never
     * behind, and its peers are left alone.
     *
     * Padding advances written[], so a source that comes back with timestamps
     * finds the hole already filled and does not fill it twice.
     */
    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        if (!m->active[i] || !m->primed[i]) continue;
        int64_t elapsed_us = now - m->start_us[i];
        int64_t content_us = av_rescale(m->written[i], 1000000, MIX_SAMPLE_RATE);
        int64_t short_us   = elapsed_us - content_us;
        if (short_us < MIX_SILENCE_GRACE_US) continue;
        if (m->debug)
            fprintf(stderr, "mixer: source %d is %lldms short of real time — "
                            "covering\n", i, (long long)(short_us / 1000));
        mixer_pad_silence_locked(m, (MixSource)i,
                                 av_rescale(short_us, MIX_SAMPLE_RATE, 1000000));
    }

    /* Loop: a large run of inserted silence can exceed one output frame, and
       leaving the remainder queued would just defer the same work to the next
       feed.  Each pass consumes n_ready from every live FIFO, so the min
       strictly decreases and this terminates. */
    for (;;) {
        int n_ready = -1;
        for (int i = 0; i < MIX_SRC_COUNT; i++) {
            if (!m->active[i] || !m->primed[i]) continue;
            int sz = av_audio_fifo_size(m->fifo[i]);
            if (n_ready < 0 || sz < n_ready) n_ready = sz;
        }
        if (n_ready <= 0) return;
        if (n_ready > m->fifo_cap) n_ready = m->fifo_cap;

        float *acc0 = (float *)m->mixed->data[0];
        float *acc1 = (float *)m->mixed->data[1];
        memset(acc0, 0, (size_t)n_ready * sizeof(float));
        memset(acc1, 0, (size_t)n_ready * sizeof(float));

        for (int i = 0; i < MIX_SRC_COUNT; i++) {
            if (!m->active[i] || !m->primed[i]) continue;
            void *dst[MIX_CHANNELS] = { m->scratch[0], m->scratch[1] };
            /* n_ready came from the shallowest FIFO, so a short read is not
               possible.  Bail rather than continue if it ever becomes so: the
               loop's exit depends on every live FIFO actually being consumed. */
            if (av_audio_fifo_read(m->fifo[i], dst, n_ready) < n_ready) return;
            const float *s0 = m->scratch[0];
            const float *s1 = m->scratch[1];
            for (int n = 0; n < n_ready; n++) { acc0[n] += s0[n]; acc1[n] += s1[n]; }
        }

        for (int n = 0; n < n_ready; n++) { acc0[n] = clampf(acc0[n]); acc1[n] = clampf(acc1[n]); }

        m->mixed->nb_samples  = n_ready;
        m->mixed->sample_rate = MIX_SAMPLE_RATE;
        if (m->sink) m->sink(m->user, m->mixed);
    }
}

MixerCtx *mixer_create(const int active[MIX_SRC_COUNT],
                       MixSinkFn sink, void *user)
{
    int any = 0;
    for (int i = 0; i < MIX_SRC_COUNT; i++) if (active[i]) any = 1;
    if (!any) return NULL;

    MixerCtx *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->sink     = sink;
    m->user     = user;
    m->now_us   = mixer_default_now_us;
    m->debug    = getenv("SCREENCAST_DEBUG") != NULL;
    m->fifo_cap  = MIX_SAMPLE_RATE * MIX_FIFO_CAP_MS / 1000;
    m->bound_cap = MIX_SAMPLE_RATE * MIX_BOUND_MS / 1000;
    pthread_mutex_init(&m->lock, NULL);

    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        m->active[i] = active[i] ? 1 : 0;
        /* 0 is a perfectly good timestamp, so "no anchor yet" needs its own
           value rather than the one calloc happened to leave behind. */
        m->anchor_us[i] = AV_NOPTS_VALUE;
        if (!m->active[i]) continue;
        m->fifo[i] = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, MIX_CHANNELS,
                                         m->fifo_cap);
        if (!m->fifo[i]) { mixer_destroy(m); return NULL; }
    }

    for (int c = 0; c < MIX_CHANNELS; c++) {
        m->scratch[c] = av_malloc(sizeof(float) * (size_t)m->fifo_cap);
        if (!m->scratch[c]) { mixer_destroy(m); return NULL; }
    }

    m->mixed = av_frame_alloc();
    if (!m->mixed) { mixer_destroy(m); return NULL; }
    m->mixed->format      = AV_SAMPLE_FMT_FLTP;
    m->mixed->sample_rate = MIX_SAMPLE_RATE;
    m->mixed->nb_samples  = m->fifo_cap;
    av_channel_layout_default(&m->mixed->ch_layout, MIX_CHANNELS);
    if (av_frame_get_buffer(m->mixed, 0) < 0) { mixer_destroy(m); return NULL; }

    return m;
}

int mixer_feed(MixerCtx *m, MixSource src, AVFrame *raw,
               int in_sample_rate, const AVChannelLayout *in_layout,
               enum AVSampleFormat in_fmt, int64_t pts_us)
{
    if (!m || src < 0 || src >= MIX_SRC_COUNT || !m->active[src] || !raw)
        return 0;

    /* Pulse/ALSA frames often carry AV_CHANNEL_ORDER_UNSPEC — stamp a native
     * layout with the captured channel count so swr accepts them. */
    int in_ch = (in_layout && in_layout->nb_channels > 0)
                ? in_layout->nb_channels
                : raw->ch_layout.nb_channels;
    if (in_ch <= 0) in_ch = 2;
    av_channel_layout_uninit(&raw->ch_layout);
    av_channel_layout_default(&raw->ch_layout, in_ch);
    if (raw->sample_rate <= 0) raw->sample_rate = in_sample_rate;

    pthread_mutex_lock(&m->lock);
    int ret = 0;

    if (!m->swr[src]) {
        AVChannelLayout out_layout, src_layout;
        av_channel_layout_default(&out_layout, MIX_CHANNELS);
        av_channel_layout_default(&src_layout, in_ch);
        ret = swr_alloc_set_opts2(&m->swr[src],
                &out_layout, AV_SAMPLE_FMT_FLTP, MIX_SAMPLE_RATE,
                &src_layout, in_fmt,             in_sample_rate,
                0, NULL);
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&src_layout);
        if (ret < 0 || swr_init(m->swr[src]) < 0) {
            log_err("swr init", ret < 0 ? ret : AVERROR_EXTERNAL);
            pthread_mutex_unlock(&m->lock);
            return ret < 0 ? ret : AVERROR_EXTERNAL;
        }
    }

    AVFrame *res = av_frame_alloc();
    if (!res) { pthread_mutex_unlock(&m->lock); return AVERROR(ENOMEM); }
    res->format      = AV_SAMPLE_FMT_FLTP;
    res->sample_rate = MIX_SAMPLE_RATE;
    av_channel_layout_default(&res->ch_layout, MIX_CHANNELS);

    ret = swr_convert_frame(m->swr[src], res, raw);
    if (ret == AVERROR_INPUT_CHANGED) {
        av_frame_unref(res);
        res->format      = AV_SAMPLE_FMT_FLTP;
        res->sample_rate = MIX_SAMPLE_RATE;
        av_channel_layout_default(&res->ch_layout, MIX_CHANNELS);
        ret = swr_convert_frame(m->swr[src], res, raw);
    }
    if (ret < 0) {
        log_err("swr_convert_frame", ret);
        av_frame_free(&res);
        pthread_mutex_unlock(&m->lock);
        return ret;
    }

    if (res->nb_samples > 0) {
        /*
         * A source that carries timestamps tells us directly when samples went
         * missing: the buffer's PTS runs ahead of where its own sample count
         * says it should be.  Writing it straight after the previous buffer
         * would close that hole by playing everything after it early, for the
         * rest of the recording, so the hole is made explicit as silence.
         *
         * Sources with no timestamps (the PipeWire capture path) pass
         * AV_NOPTS_VALUE and simply count samples, as they always did.
         */
        if (pts_us != AV_NOPTS_VALUE) {
            if (m->anchor_us[src] == AV_NOPTS_VALUE) {
                m->anchor_us[src] = pts_us;
                m->written[src]   = 0;
            } else {
                int64_t expected = m->anchor_us[src] +
                    av_rescale(m->written[src], 1000000, MIX_SAMPLE_RATE);
                int64_t gap_us = pts_us - expected;
                if (gap_us >= MIX_GAP_MIN_US) {
                    if (m->debug)
                        fprintf(stderr, "mixer: source %d skipped %lldms — "
                                        "inserting silence\n",
                                src, (long long)(gap_us / 1000));
                    mixer_pad_silence_locked(m, src,
                        av_rescale(gap_us, MIX_SAMPLE_RATE, 1000000));
                }
            }
        }

        /* written[] is the ledger every later gap is measured against, so it
           may only advance by what the FIFO actually took. */
        int put = av_audio_fifo_write(m->fifo[src], (void **)res->data,
                                      res->nb_samples);
        if (put > 0) m->written[src] += put;
        if (!m->primed[src]) {
            m->primed[src]  = 1;
            m->start_us[src] = m->now_us();
            if (m->debug)
                fprintf(stderr, "mixer: source %d primed (first samples)\n", src);
        }
        m->last_feed_us[src] = m->now_us();
    }
    av_frame_free(&res);

    mixer_drain_locked(m);

    /*
     * Last-resort bound, applied only after the drain.  What a FIFO still holds
     * at this point is its lead over the slowest live source, and the lockstep
     * repair in the drain exists to keep that well under the cap.  Reaching
     * here means it failed to, and the samples dropped shift the track against
     * the video — so say so rather than lose them quietly.
     */
    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        if (!m->fifo[i]) continue;
        int overflow = av_audio_fifo_size(m->fifo[i]) - m->bound_cap;
        if (overflow > 0) {
            fprintf(stderr, "mixer: source %d overran its buffer — dropping "
                            "%d samples (the track will shift)\n", i, overflow);
            av_audio_fifo_drain(m->fifo[i], overflow);
        }
    }

    pthread_mutex_unlock(&m->lock);
    return 0;
}

void mixer_drop_source(MixerCtx *m, MixSource src)
{
    if (!m || src < 0 || src >= MIX_SRC_COUNT) return;

    pthread_mutex_lock(&m->lock);
    if (m->active[src]) {
        m->active[src] = 0;
        /* Discard the dead source's leftover samples so they can't be mixed,
         * then flush what the remaining sources were holding back in lockstep. */
        if (m->fifo[src])
            av_audio_fifo_drain(m->fifo[src], av_audio_fifo_size(m->fifo[src]));
        mixer_drain_locked(m);
    }
    pthread_mutex_unlock(&m->lock);
}

int mixer_source_live(MixerCtx *m, MixSource src)
{
    if (!m || src < 0 || src >= MIX_SRC_COUNT) return 0;
    pthread_mutex_lock(&m->lock);
    int live = m->active[src] && m->primed[src];
    pthread_mutex_unlock(&m->lock);
    return live;
}

void mixer_set_clock(MixerCtx *m, int64_t (*now_us)(void))
{
    if (!m || !now_us) return;
    pthread_mutex_lock(&m->lock);
    m->now_us = now_us;
    for (int i = 0; i < MIX_SRC_COUNT; i++) m->start_us[i] = now_us();
    pthread_mutex_unlock(&m->lock);
}

void mixer_destroy(MixerCtx *m)
{
    if (!m) return;
    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        swr_free(&m->swr[i]);
        if (m->fifo[i]) av_audio_fifo_free(m->fifo[i]);
    }
    for (int c = 0; c < MIX_CHANNELS; c++) av_free(m->scratch[c]);
    av_frame_free(&m->mixed);
    pthread_mutex_destroy(&m->lock);
    free(m);
}
