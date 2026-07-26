/*
 * Mixer unit tests — timeline continuity under source stalls and gaps.
 *
 * The mixer is the one place where a lost or late audio buffer can shift the
 * whole track against the video, permanently: audio PTS is derived from a
 * running sample count, so a sample that never arrives is not a glitch, it is
 * an offset that every later sample inherits.
 *
 * Two rules are pinned here, and the second matters as much as the first: a gap
 * a source reports through its timestamps becomes silence, and a gap in *one*
 * source never becomes a shift of the whole mix.
 *
 * Everything is deterministic: the tests drive the mixer's clock directly
 * (mixer_set_clock), so elapsed time is exact and nothing sleeps.
 */

#include "mixer.h"

#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR          MIX_SAMPLE_RATE
#define MS(n)       ((n) * SR / 1000)          /* milliseconds → samples */
#define US(n)       ((int64_t)(n) * 1000LL)    /* milliseconds → µs */

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
        }                                                                     \
    } while (0)

/* ── sink that records everything it is handed ─────────────── */

#define SINK_CAP (SR * 10)   /* 10 s is far more than any test produces */

typedef struct {
    float *ch[MIX_CHANNELS];
    int    n;
    int    calls;
} Sink;

static Sink g_sink;

static void sink_reset(void)
{
    for (int c = 0; c < MIX_CHANNELS; c++) {
        if (!g_sink.ch[c]) g_sink.ch[c] = malloc(sizeof(float) * SINK_CAP);
        memset(g_sink.ch[c], 0, sizeof(float) * SINK_CAP);
    }
    g_sink.n = 0;
    g_sink.calls = 0;
}

static void sink_fn(void *user, AVFrame *mixed)
{
    (void)user;
    g_sink.calls++;
    int n = mixed->nb_samples;
    if (g_sink.n + n > SINK_CAP) { CHECK(0, "sink overflow"); return; }
    for (int c = 0; c < MIX_CHANNELS; c++)
        memcpy(g_sink.ch[c] + g_sink.n, mixed->data[c], sizeof(float) * (size_t)n);
    g_sink.n += n;
}

/* Every sample in [from,to) equals `want` on both channels. */
static int sink_region_is(int from, int to, float want)
{
    for (int c = 0; c < MIX_CHANNELS; c++)
        for (int i = from; i < to; i++)
            if (fabsf(g_sink.ch[c][i] - want) > 1e-4f) {
                fprintf(stderr, "  (ch%d[%d] = %f, wanted %f)\n",
                        c, i, g_sink.ch[c][i], want);
                return 0;
            }
    return 1;
}

/* ── controllable clock ────────────────────────────────────── */

/*
 * The mixer decides a source has gone quiet by elapsed time, so the tests own
 * the clock rather than sleeping through it.  Feeds advance it by the duration
 * of the audio they carry, which is what a real capture does.
 */
static int64_t g_now_us;
static int64_t test_now_us(void) { return g_now_us; }

/* Let `ms` of recording time pass. */
static void advance(int ms) { g_now_us += (int64_t)ms * 1000; }

/* ── input helper ──────────────────────────────────────────── */

/* One canonical-format frame of `n` samples holding a constant value. */
static AVFrame *tone(int n, float value)
{
    AVFrame *f = av_frame_alloc();
    f->format      = AV_SAMPLE_FMT_FLTP;
    f->sample_rate = SR;
    f->nb_samples  = n;
    av_channel_layout_default(&f->ch_layout, MIX_CHANNELS);
    if (av_frame_get_buffer(f, 0) < 0) { fprintf(stderr, "oom\n"); exit(1); }
    for (int c = 0; c < MIX_CHANNELS; c++) {
        float *p = (float *)f->data[c];
        for (int i = 0; i < n; i++) p[i] = value;
    }
    return f;
}

static void feed(MixerCtx *m, MixSource src, int n, float value, int64_t pts_us)
{
    AVFrame *f = tone(n, value);
    AVChannelLayout ly;
    av_channel_layout_default(&ly, MIX_CHANNELS);
    int ret = mixer_feed(m, src, f, SR, &ly, AV_SAMPLE_FMT_FLTP, pts_us);
    CHECK(ret == 0, "mixer_feed returned %d", ret);
    av_channel_layout_uninit(&ly);
    av_frame_free(&f);
}

/* ── tests ─────────────────────────────────────────────────── */

/*
 * A single source that skips 90 ms between two buffers must have that 90 ms
 * appear in the output as silence.  Without it the second buffer is emitted
 * back-to-back with the first and everything after it plays 90 ms early — for
 * the rest of the recording.
 */
static void test_gap_becomes_silence(void)
{
    fprintf(stderr, "test_gap_becomes_silence\n");
    sink_reset();

    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);
    g_now_us = 0;
    mixer_set_clock(m, test_now_us);

    feed(m, MIX_SRC_MIC, MS(10), 0.5f, 0);
    CHECK(g_sink.n == MS(10), "expected %d samples, got %d", MS(10), g_sink.n);

    /* Next buffer claims to start at 100 ms; 10 ms was already delivered, so
       90 ms of the timeline is missing and must be filled. */
    feed(m, MIX_SRC_MIC, MS(10), 0.5f, US(100));

    int want = MS(10) + MS(90) + MS(10);
    CHECK(g_sink.n == want, "expected %d samples, got %d", want, g_sink.n);
    CHECK(sink_region_is(0, MS(10), 0.5f), "first buffer not intact");
    CHECK(sink_region_is(MS(10), MS(100), 0.0f), "gap was not silence");
    CHECK(sink_region_is(MS(100), want, 0.5f), "second buffer not intact");

    mixer_destroy(m);
}

/*
 * Small jitter is not a gap.  Capture buffers never land exactly on their
 * nominal timestamps, and padding a millisecond of silence per buffer would
 * itself be the drift it is trying to prevent.
 */
static void test_jitter_is_not_a_gap(void)
{
    fprintf(stderr, "test_jitter_is_not_a_gap\n");
    sink_reset();

    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);
    g_now_us = 0;
    mixer_set_clock(m, test_now_us);

    /* Ten 10 ms buffers, each arriving 2 ms later than nominal. */
    for (int i = 0; i < 10; i++)
        feed(m, MIX_SRC_MIC, MS(10), 0.25f, US(10 * i) + 2000);

    CHECK(g_sink.n == MS(100), "expected %d samples, got %d", MS(100), g_sink.n);
    CHECK(sink_region_is(0, g_sink.n, 0.25f), "jitter introduced silence");

    mixer_destroy(m);
}

/*
 * A source with no timestamps at all (the PipeWire capture path) must behave
 * exactly as it did before timestamps existed: sample counting, no padding.
 */
static void test_no_pts_never_pads(void)
{
    fprintf(stderr, "test_no_pts_never_pads\n");
    sink_reset();

    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);
    g_now_us = 0;
    mixer_set_clock(m, test_now_us);

    for (int i = 0; i < 5; i++)
        feed(m, MIX_SRC_MIC, MS(10), 0.5f, AV_NOPTS_VALUE);

    CHECK(g_sink.n == MS(50), "expected %d samples, got %d", MS(50), g_sink.n);
    CHECK(sink_region_is(0, g_sink.n, 0.5f), "unexpected silence");

    mixer_destroy(m);
}

/*
 * The one the whole change exists for.  Two sources are mixed in lockstep at
 * min(available); a source that stops delivering pins that minimum at zero and
 * the other's samples pile up behind it.  The buffer bound must outlast the
 * stall, because a discarded audio sample is not a dropout — it is every later
 * sample playing early for the rest of the recording.
 *
 * A full second of stall is five times the old 200 ms bound, so nothing here
 * could have survived it before.
 */
static void test_quiet_source_does_not_freeze_the_mix(void)
{
    fprintf(stderr, "test_quiet_source_does_not_freeze_the_mix\n");
    sink_reset();

    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1, [MIX_SRC_DESKTOP] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);
    g_now_us = 0;
    mixer_set_clock(m, test_now_us);

    feed(m, MIX_SRC_MIC,     MS(10), 0.5f,  0);
    feed(m, MIX_SRC_DESKTOP, MS(10), 0.25f, 0);

    /* Desktop audio goes quiet — nothing is playing — while the mic runs on
       for a full second of recording time. */
    for (int i = 1; i <= 100; i++) {
        advance(10);
        feed(m, MIX_SRC_MIC, MS(10), 0.5f, US(10 * i));
    }

    /*
     * The mix must have kept moving throughout.  It previously emitted nothing
     * at all here: min() sat at zero for the whole second, and because the
     * muxer holds video until audio covers the same span, the picture froze
     * for a second with it.
     */
    CHECK(g_sink.n >= MS(1010) - MS(120),
          "mix stalled behind the quiet source: %d samples for 1010ms",
          g_sink.n);
    CHECK(g_sink.n <= MS(1010), "mix ran ahead of real time: %d samples "
          "for 1010ms", g_sink.n);

    /* The mic is carried at unity — the quiet source contributed silence, not
       a hole and not its last buffer repeated. */
    CHECK(sink_region_is(MS(20), g_sink.n, 0.5f),
          "quiet source contributed something other than silence");

    mixer_destroy(m);
}

/*
 * Regression: one source's gap must never become the whole mix's shift.
 *
 * A device can drop samples and carry on delivering at cadence — the buffer it
 * hands over next simply carries a timestamp that has jumped.  The silence that
 * fills that jump makes the source's buffer deeper without any time having
 * passed, so anything that judged "how far behind is this source" by buffer
 * depth would read the *healthy* source as lagging and pad it to match.
 *
 * That is a desync of exactly the gap's length, applied to a source that lost
 * nothing.  Only a source's own timestamps may move it on the timeline.
 */
static void test_one_sources_gap_is_not_the_mixs_shift(void)
{
    fprintf(stderr, "test_one_sources_gap_is_not_the_mixs_shift\n");
    sink_reset();

    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1, [MIX_SRC_DESKTOP] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);
    g_now_us = 0;
    mixer_set_clock(m, test_now_us);

    for (int i = 0; i < 10; i++) {
        advance(10);
        feed(m, MIX_SRC_MIC,     MS(10), 0.5f,  US(10 * i));
        feed(m, MIX_SRC_DESKTOP, MS(10), 0.25f, US(10 * i));
    }

    /* The mic drops half a second but keeps delivering; desktop is untouched
       and its timestamps say so.  Only 100 ms more of real time passes. */
    for (int i = 10; i < 20; i++) {
        advance(10);
        feed(m, MIX_SRC_MIC,     MS(10), 0.5f,  US(10 * i) + 500000);
        feed(m, MIX_SRC_DESKTOP, MS(10), 0.25f, US(10 * i));
    }

    /* 200 ms of recording happened, so about 200 ms may come out.  Padding the
       healthy source too would put ~700 ms here. */
    CHECK(g_sink.n <= MS(210), "one source's 500ms gap shifted the whole mix: "
          "%d samples emitted for a 200ms timeline", g_sink.n);
    CHECK(g_sink.n >= MS(190), "lost samples: %d for a 200ms timeline",
          g_sink.n);

    mixer_destroy(m);
}

/*
 * The freeze regression, in the shape that actually occurred.
 *
 * A source that delivers steadily but short of real time — desktop audio, which
 * only has samples while something is playing — drags min() down with it.  The
 * other source then backs up against its bound and the whole mix falls behind
 * real time.  Nothing is audible, because late audio still plays back intact;
 * the cost lands on video, which the muxer holds back to match.
 */
static void test_short_source_does_not_drag_the_mix_late(void)
{
    fprintf(stderr, "test_short_source_does_not_drag_the_mix_late\n");
    sink_reset();

    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1, [MIX_SRC_DESKTOP] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);
    g_now_us = 0;
    mixer_set_clock(m, test_now_us);

    feed(m, MIX_SRC_MIC,     MS(10), 0.5f,  0);
    feed(m, MIX_SRC_DESKTOP, MS(10), 0.25f, 0);

    /* Two seconds of recording.  The mic keeps up; desktop delivers on every
       tick but only half a tick's worth of audio each time. */
    for (int i = 1; i <= 200; i++) {
        advance(10);
        feed(m, MIX_SRC_MIC,     MS(10), 0.5f,  US(10 * i));
        feed(m, MIX_SRC_DESKTOP, MS(5),  0.25f, AV_NOPTS_VALUE);
    }

    /*
     * The mix must track real time to within the grace.  Unbounded, it ends up
     * a full second behind — and a second of audio lag is a second of frozen
     * video once the muxer interleaves it.
     */
    int64_t behind = MS(2010) - g_sink.n;
    CHECK(behind <= MS(120), "mix fell %lldms behind real time",
          (long long)(behind * 1000 / SR));

    mixer_destroy(m);
}

int main(void)
{
    test_gap_becomes_silence();
    test_jitter_is_not_a_gap();
    test_no_pts_never_pads();
    test_quiet_source_does_not_freeze_the_mix();
    test_one_sources_gap_is_not_the_mixs_shift();
    test_short_source_does_not_drag_the_mix_late();

    for (int c = 0; c < MIX_CHANNELS; c++) free(g_sink.ch[c]);

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "\nall mixer tests passed\n");
    return 0;
}
