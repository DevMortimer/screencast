#include "mixer.h"
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
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

/* ~200 ms per source: absorbs jitter, bounds drift and memory. */
#define MIX_FIFO_CAP_MS 200

struct MixerCtx {
    int             active[MIX_SRC_COUNT];
    SwrContext     *swr[MIX_SRC_COUNT];   /* source → canonical, lazily built */
    AVAudioFifo    *fifo[MIX_SRC_COUNT];  /* canonical stereo FLTP, bounded */
    int             fifo_cap;             /* samples */
    float          *scratch[MIX_CHANNELS];/* per-drain read buffer */
    AVFrame        *mixed;                /* reused canonical output frame */
    MixSinkFn       sink;
    void           *user;
    pthread_mutex_t lock;
};

static float clampf(float v)
{
    if (v >  1.0f) return  1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

/*
 * Emit min(available) samples across the active sources, summed at unity gain
 * and clamped. Called with m->lock held. The min() keeps the two streams in
 * lockstep; the bounded FIFOs (enforced in mixer_feed) keep the faster source
 * from running away.
 */
static void mixer_drain_locked(MixerCtx *m)
{
    int n_ready = -1;
    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        if (!m->active[i]) continue;
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
        if (!m->active[i]) continue;
        void *dst[MIX_CHANNELS] = { m->scratch[0], m->scratch[1] };
        if (av_audio_fifo_read(m->fifo[i], dst, n_ready) < n_ready) continue;
        const float *s0 = m->scratch[0];
        const float *s1 = m->scratch[1];
        for (int n = 0; n < n_ready; n++) { acc0[n] += s0[n]; acc1[n] += s1[n]; }
    }

    for (int n = 0; n < n_ready; n++) { acc0[n] = clampf(acc0[n]); acc1[n] = clampf(acc1[n]); }

    m->mixed->nb_samples  = n_ready;
    m->mixed->sample_rate = MIX_SAMPLE_RATE;
    if (m->sink) m->sink(m->user, m->mixed);
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
    m->fifo_cap = MIX_SAMPLE_RATE * MIX_FIFO_CAP_MS / 1000;
    pthread_mutex_init(&m->lock, NULL);

    for (int i = 0; i < MIX_SRC_COUNT; i++) {
        m->active[i] = active[i] ? 1 : 0;
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
               enum AVSampleFormat in_fmt)
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
        av_audio_fifo_write(m->fifo[src], (void **)res->data, res->nb_samples);
        /* Bound the FIFO: drop the oldest samples of the faster source. */
        int overflow = av_audio_fifo_size(m->fifo[src]) - m->fifo_cap;
        if (overflow > 0) av_audio_fifo_drain(m->fifo[src], overflow);
    }
    av_frame_free(&res);

    mixer_drain_locked(m);
    pthread_mutex_unlock(&m->lock);
    return 0;
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
