/*
 * Audio source worker tests — the feed-loop policy in one place.
 *
 * The worker owns what used to be three nearly identical thread bodies (one
 * per platform main, and on macOS one per source): keep feeding the mixer
 * while the session runs, back off on errors, and drop the source from the
 * mix after a run of consecutive failures so it cannot hold the remaining
 * sources hostage in the lockstep.
 *
 * The tests drive the worker through its interface with a scripted fake
 * reader and a real mixer (the mixer is the worker's one real dependency —
 * feeding it is the whole point), so the drop policy, the error-run reset,
 * and the stop handshakes are all pinned without any device.
 */

#include "audsrc.h"
#include "mixer.h"

#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR  MIX_SAMPLE_RATE

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

/* ── sink that counts what the mixer emits ────────────────── */

typedef struct {
    int calls;
    int samples;   /* total samples across both channels */
} Sink;

static Sink g_sink;

static void sink_fn(void *user, AVFrame *mixed)
{
    (void)user;
    g_sink.calls++;
    g_sink.samples += mixed->nb_samples;
}

/* ── scripted fake reader ─────────────────────────────────── */

typedef struct {
    AudSrcResult *script;   /* results to serve, one per read */
    int           len;
    int           reads;    /* reads served so far */
} Fake;

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

static AudSrcRead frame_read(void *user)
{
    Fake *f = user;
    if (f->reads >= f->len)
        return (AudSrcRead){ .result = AUDSRC_STOP };   /* script exhausted */
    int i = f->reads;
    f->reads++;
    if (f->script[i] != AUDSRC_FRAME)
        return (AudSrcRead){ .result = f->script[i] };
    return (AudSrcRead){
        .result = AUDSRC_FRAME,
        .frame  = tone(480, 0.5f),     /* 10 ms */
        .owned  = 1,
        .in_sample_rate = SR,
        .in_channels    = MIX_CHANNELS,
        .in_fmt         = AV_SAMPLE_FMT_FLTP,
        .pts_us         = (int64_t)(f->reads - 1) * 10000LL,
    };
}

/* ── controllable running predicate ───────────────────────── */

static int g_stop_after_reads = INT_MAX;   /* reads served before stopping */

static int test_running(void *user)
{
    Fake *f = user;
    return f->reads < g_stop_after_reads;
}

/* Run the worker over a script; returns the number of scripted reads served.
   The emitted mix lands in the global sink (sink_fn is wired to it). */
static int run_script(AudSrcResult *script, int len, int max_fails,
                      long error_backoff_ns, long empty_poll_ns)
{
    int active[MIX_SRC_COUNT] = { [MIX_SRC_MIC] = 1 };
    MixerCtx *m = mixer_create(active, sink_fn, NULL);
    assert(m);

    Fake fake = { .script = script, .len = len };
    AudSrcWorker w = {
        .src = MIX_SRC_MIC, .mixer = m, .label = "microphone",
        .max_fails = max_fails,
        .error_backoff_ns = error_backoff_ns,
        .empty_poll_ns    = empty_poll_ns,
        .read = frame_read, .running = test_running, .user = &fake,
    };

    g_sink.calls = 0;
    g_sink.samples = 0;

    audsrc_run(&w);
    int reads = fake.reads;

    mixer_destroy(m);
    return reads;
}

/* ── tests ────────────────────────────────────────────────── */

/* Frames must flow straight from the reader to the mixer, and a healthy
   source stays in the mix. */
static void test_frames_reach_the_mix(void)
{
    fprintf(stderr, "test_frames_reach_the_mix\n");
    AudSrcResult script[] = { AUDSRC_FRAME, AUDSRC_FRAME, AUDSRC_FRAME };
    g_stop_after_reads = INT_MAX;

    int reads = run_script(script, 3, 3, 1, 1);

    CHECK(reads == 3, "expected 3 reads, got %d", reads);
    CHECK(g_sink.samples == 3 * 480, "expected 1440 mixed samples, got %d",
          g_sink.samples);
}

/* A source that only ever errors is dropped after max_fails consecutive
   failures, and the worker stops reading it immediately after. */
static void test_errors_drop_the_source(void)
{
    fprintf(stderr, "test_errors_drop_the_source\n");
    AudSrcResult script[] = { AUDSRC_ERROR, AUDSRC_ERROR, AUDSRC_ERROR,
                              AUDSRC_ERROR, AUDSRC_ERROR };
    g_stop_after_reads = INT_MAX;

    int reads = run_script(script, 5, 3, 1, 1);

    CHECK(reads == 3, "expected exactly 3 reads before the drop, got %d",
          reads);
    CHECK(g_sink.samples == 0, "errors must not reach the mix (%d samples)",
          g_sink.samples);
}

/* One delivered frame ends the error run: the counter restarts, so a source
   that recovers is dropped only after max_fails *fresh* errors. */
static void test_frame_resets_the_error_run(void)
{
    fprintf(stderr, "test_frame_resets_the_error_run\n");
    AudSrcResult script[] = { AUDSRC_ERROR, AUDSRC_ERROR, AUDSRC_FRAME,
                              AUDSRC_ERROR, AUDSRC_ERROR, AUDSRC_ERROR };
    g_stop_after_reads = INT_MAX;

    int reads = run_script(script, 6, 3, 1, 1);

    CHECK(reads == 6, "expected 6 reads (3 errors, frame, 3 errors), got %d",
          reads);
    CHECK(g_sink.samples == 480, "expected the one frame in the mix, got %d",
          g_sink.samples);
}

/* An EMPTY read (a source with nothing queued right now — desktop audio
   while nothing plays) is not a failure: it neither counts toward the drop
   nor resets the error run. */
static void test_empty_reads_never_count(void)
{
    fprintf(stderr, "test_empty_reads_never_count\n");
    AudSrcResult script[] = { AUDSRC_ERROR, AUDSRC_EMPTY, AUDSRC_EMPTY,
                              AUDSRC_ERROR, AUDSRC_ERROR };
    g_stop_after_reads = INT_MAX;

    int reads = run_script(script, 5, 3, 1, 1);

    CHECK(reads == 5, "expected 5 reads (drop on the 3rd error), got %d",
          reads);
    CHECK(g_sink.samples == 0, "no frames in this script (%d samples)",
          g_sink.samples);
}

/* A reader that reports the session is over ends the loop cleanly — no
   drop, no backoff, nothing fed. */
static void test_stop_read_ends_the_loop(void)
{
    fprintf(stderr, "test_stop_read_ends_the_loop\n");
    AudSrcResult script[] = { AUDSRC_FRAME, AUDSRC_FRAME, AUDSRC_STOP,
                              AUDSRC_FRAME };
    g_stop_after_reads = INT_MAX;

    int reads = run_script(script, 4, 3, 1, 1);

    CHECK(reads == 3, "expected 3 reads before STOP, got %d", reads);
    CHECK(g_sink.samples == 2 * 480, "expected 2 frames in the mix, got %d",
          g_sink.samples);
}

/* The running predicate is checked before every read: when the session ends
   (s_rec_open drops) the loop stops without touching the reader again. */
static void test_running_false_ends_the_loop(void)
{
    fprintf(stderr, "test_running_false_ends_the_loop\n");
    AudSrcResult script[] = { AUDSRC_FRAME, AUDSRC_FRAME, AUDSRC_FRAME };
    g_stop_after_reads = 2;

    int reads = run_script(script, 3, 3, 1, 1);

    CHECK(reads == 2, "expected 2 reads before the predicate went false, "
          "got %d", reads);
    CHECK(g_sink.samples == 2 * 480, "expected 2 frames in the mix, got %d",
          g_sink.samples);
}

/* The backoff between errors is honoured: two errors with a 5 ms backoff
   cannot complete in under 8 ms of wall time. */
static void test_error_backoff_between_reads(void)
{
    fprintf(stderr, "test_error_backoff_between_reads\n");
    AudSrcResult script[] = { AUDSRC_ERROR, AUDSRC_ERROR, AUDSRC_ERROR };
    g_stop_after_reads = INT_MAX;

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    run_script(script, 3, 3, 5 * 1000000L, 1);
    clock_gettime(CLOCK_MONOTONIC, &b);

    int64_t elapsed_us = (int64_t)(b.tv_sec - a.tv_sec) * 1000000LL +
                         (b.tv_nsec - a.tv_nsec) / 1000;
    CHECK(elapsed_us >= 8000, "two 5ms backoffs took only %lldus",
          (long long)elapsed_us);
}

int main(void)
{
    test_frames_reach_the_mix();
    test_errors_drop_the_source();
    test_frame_resets_the_error_run();
    test_empty_reads_never_count();
    test_stop_read_ends_the_loop();
    test_running_false_ends_the_loop();
    test_error_backoff_between_reads();

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "\nall audsrc tests passed\n");
    return 0;
}
