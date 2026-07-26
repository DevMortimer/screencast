#pragma once
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

/*
 * Live audio mixer: combines microphone and desktop (monitor) capture into a
 * single canonical stream and delivers it to a sink (the encoder).
 *
 * Each source is resampled to the canonical format into its own bounded FIFO.
 * On every feed the mixer emits min(available) samples across the live
 * sources, summed at unity gain and hard-clamped — so the two streams stay in
 * lockstep and neither FIFO can grow without bound over a long recording
 * (see docs/adr/0001-in-process-desktop-mic-mix.md).
 *
 * "Live" means active *and* primed: a source counts toward the min() only after
 * its first delivered sample, and is dropped if it then stalls.  Without this a
 * single silent source zeroes the min() and the whole mixed track disappears.
 *
 * Gaps become silence, never a shift.  A source that skips samples — reported
 * by its timestamps — or that simply stops delivering for a while has the
 * missing span written into its FIFO as silence, so the mix keeps flowing and
 * the sources that are still running lose nothing.  The alternative is what
 * bounded FIFOs do left to themselves: discard the live source's oldest samples
 * to make room, which does not sound like a dropout, it sounds like every
 * remaining sample arriving early against the video, for the rest of the take.
 */

/* Canonical mix format: stereo FLTP at 48 kHz (clean AAC input). */
#define MIX_SAMPLE_RATE 48000
#define MIX_CHANNELS    2

typedef enum {
    MIX_SRC_MIC     = 0,
    MIX_SRC_DESKTOP = 1,
    MIX_SRC_COUNT   = 2,
} MixSource;

/* Receives mixed stereo-FLTP frames at MIX_SAMPLE_RATE. */
typedef void (*MixSinkFn)(void *user, AVFrame *mixed);

typedef struct MixerCtx MixerCtx;

/*
 * Create a mixer. active[i] != 0 marks source i as one that is *expected* to be
 * fed.  A source only joins the min()-lockstep once it has actually delivered
 * samples, so an expected source that never materialises does not suppress the
 * mix.  Returns NULL on allocation failure or when no source is active.
 */
MixerCtx *mixer_create(const int active[MIX_SRC_COUNT],
                       MixSinkFn sink, void *user);

/*
 * Feed one raw capture frame for `src` (any rate/layout/format). Resamples to
 * the canonical format, buffers it, and drains any mixable samples to the sink.
 * Thread-safe: mic and desktop threads may call concurrently for their own src.
 *
 * `pts_us` is the capture timestamp of the frame's *first* sample, on whatever
 * clock the source uses — only the differences between one call and the next
 * matter, so the mixer anchors itself on the first value it sees.  A source
 * that has no timestamps passes AV_NOPTS_VALUE and is counted by samples alone.
 *
 * Timestamps are what let a hole in a source become silence rather than an
 * offset.  The audio track's PTS is a running sample count, so samples a device
 * failed to deliver do not play back as a dropout; they play back as every
 * later sample arriving early, permanently, against the video.
 */
int  mixer_feed(MixerCtx *m, MixSource src, AVFrame *raw,
                int in_sample_rate, const AVChannelLayout *in_layout,
                enum AVSampleFormat in_fmt, int64_t pts_us);

/*
 * Drop `src` from the mix (e.g. its capture device died mid-recording).  The
 * source stops counting toward the min()-lockstep, so the mixed track keeps
 * flowing over whatever sources remain.  Idempotent and thread-safe.
 */
void mixer_drop_source(MixerCtx *m, MixSource src);

/*
 * True when `src` is still in the mix *and* has actually delivered samples.
 * Lets the caller report what really made it into the track rather than what
 * was merely requested at create time.  Thread-safe.
 */
int  mixer_source_live(MixerCtx *m, MixSource src);

/*
 * Testing seam: replace the clock the mixer measures elapsed time with.
 *
 * The mixer decides when a source has gone quiet by how much time has passed,
 * which is the one thing a test cannot control without sleeping through it.
 * Call this immediately after mixer_create, and only from tests.
 */
void mixer_set_clock(MixerCtx *m, int64_t (*now_us)(void));

void mixer_destroy(MixerCtx *m);
