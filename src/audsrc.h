#pragma once
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include "mixer.h"

/*
 * Audio source worker: one thread's feed loop for one audio source.
 *
 * Every platform main used to own its own copy of this loop — the microphone
 * and desktop-audio threads in both mains — and the copies drifted: the macOS
 * desktop thread lost the drop policy entirely, while the mixer separately
 * drops stalled sources on its own timeout.  This module is that loop, once.
 *
 * The worker reads frames from a source through a reader adapter and feeds
 * them to the mixer until one of three things happens:
 *
 *   - the reader reports the session is over (AUDSRC_STOP);
 *   - the running predicate goes false (the recording closed);
 *   - the source fails AUDSRC_MAX_FAILS consecutive reads — it is dropped
 *     from the mix so it cannot hold the remaining sources hostage in the
 *     lockstep, and the loop ends.
 *
 * A read that merely has nothing queued (AUDSRC_EMPTY — desktop audio while
 * nothing plays) is neither a failure nor a reset: the worker polls it on a
 * short cadence and never counts it.
 *
 * The adapter owns the per-platform read (capture_read on Linux, avf_mic_read
 * / sck_capture_grab_audio on macOS) and any per-frame bookkeeping the main
 * wants (frame counters, the audio anchor).  The worker owns the policy.
 */

/* Drop a source after this many consecutive failed reads. */
#define AUDSRC_MAX_FAILS 40

/* Backoff between errors: bounded, never a busy-spin. */
#define AUDSRC_ERROR_BACKOFF_NS 100000000LL   /* 100 ms */

/* Poll cadence for a source that has nothing queued.  10 ms keeps desktop
 * audio latency small — the capture queue holds at most ~2 s of audio, and
 * the mixed track must not run visibly behind the video. */
#define AUDSRC_EMPTY_POLL_NS 10000000LL       /* 10 ms */

/* Outcome of one adapter read. */
typedef enum {
    AUDSRC_FRAME = 0,  /* one frame; the format fields below are valid */
    AUDSRC_EMPTY,      /* nothing queued right now; not a failure */
    AUDSRC_ERROR,      /* the source failed; counts toward the drop policy */
    AUDSRC_STOP,       /* the session is over; end the loop cleanly */
} AudSrcResult;

/* One read's worth of data, filled by the adapter. */
typedef struct {
    AudSrcResult     result;
    AVFrame         *frame;          /* set when result == AUDSRC_FRAME */
    int              owned;          /* 1 = the worker frees the frame */
    int              in_sample_rate; /* source format, passed to the mixer */
    int              in_channels;
    enum AVSampleFormat in_fmt;
    int64_t          pts_us;         /* capture time of sample 0, or
                                        AV_NOPTS_VALUE */
} AudSrcRead;

typedef AudSrcRead (*AudSrcReadFn)(void *user);
typedef int  (*AudSrcRunningFn)(void *user);   /* 0 = stop the loop */

typedef struct {
    MixSource        src;             /* which mixer source this feeds */
    MixerCtx        *mixer;
    const char      *label;           /* for the drop message */
    int              max_fails;       /* AUDSRC_MAX_FAILS in production */
    long             error_backoff_ns;/* AUDSRC_ERROR_BACKOFF_NS */
    long             empty_poll_ns;   /* AUDSRC_EMPTY_POLL_NS */
    AudSrcReadFn     read;
    AudSrcRunningFn  running;
    void            *user;            /* passed to both callbacks */
} AudSrcWorker;

/*
 * Run the feed loop on the calling thread until the source stops, is dropped,
 * or running() goes false.  The caller owns the thread.
 */
void audsrc_run(const AudSrcWorker *w);
