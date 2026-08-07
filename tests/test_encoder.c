/*
 * Encoder tests — the module through its interface, not its fields.
 *
 * The encoder used to be shallow: its header exposed every field, and the
 * platform mains reached through the seam for state (the [SYNC] telemetry,
 * the render decision).  These tests pin the accessors that replaced those
 * reads, and the timeline rules behind them: the audio track anchors on the
 * session timeline exactly once, then counts samples, so nothing after the
 * first frame can move it.
 *
 * A real encoder is opened (the same h264_videotoolbox / h264_nvenc the
 * recorder uses) so the whole path — open, feed, flush, free — is exercised
 * on every machine the project builds on.
 */

#include "encoder.h"

#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SR 48000

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

static char g_path[256];

static void make_path(void)
{
    snprintf(g_path, sizeof(g_path), "/tmp/screencast_encoder_test_%ld.mp4",
             (long)getpid());
}

/* One 1024-sample stereo FLTP frame (one AAC frame) of silence. */
static AVFrame *audio_frame(void)
{
    AVFrame *f = av_frame_alloc();
    assert(f);
    f->format      = AV_SAMPLE_FMT_FLTP;
    f->sample_rate = SR;
    f->nb_samples  = 1024;
    av_channel_layout_default(&f->ch_layout, 2);
    assert(av_frame_get_buffer(f, 0) == 0);
    for (int c = 0; c < 2; c++)
        memset(f->data[c], 0, sizeof(float) * 1024);
    return f;
}

static EncoderCtx *open_with_audio(void)
{
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);
    EncoderCtx *enc = encoder_open(g_path, 320, 240, 24, AV_PIX_FMT_BGRA,
                                   SR, &stereo, AV_SAMPLE_FMT_FLTP);
    av_channel_layout_uninit(&stereo);
    return enc;
}

/* A freshly opened encoder has written its header and its audio track sits
   at position zero — nothing fed, nothing claimed. */
static void test_open_starts_clean(void)
{
    fprintf(stderr, "test_open_starts_clean\n");
    EncoderCtx *enc = open_with_audio();
    CHECK(enc != NULL, "encoder_open failed");
    if (!enc) return;

    CHECK(encoder_header_written(enc) == 1, "header not written after open");
    CHECK(encoder_audio_pts_us(enc) == 0, "audio track not at zero, got %lld",
          (long long)encoder_audio_pts_us(enc));

    CHECK(encoder_flush(enc) == 0, "encoder_flush failed");
    encoder_free(enc);
}

/* Without an audio track there is no audio position to report. */
static void test_no_audio_track_reports_minus_one(void)
{
    fprintf(stderr, "test_no_audio_track_reports_minus_one\n");
    EncoderCtx *enc = encoder_open(g_path, 320, 240, 24, AV_PIX_FMT_BGRA,
                                   0, NULL, AV_SAMPLE_FMT_NONE);
    CHECK(enc != NULL, "encoder_open (video only) failed");
    if (!enc) return;

    CHECK(encoder_audio_pts_us(enc) == -1, "expected -1 without audio, got %lld",
          (long long)encoder_audio_pts_us(enc));

    encoder_flush(enc);
    encoder_free(enc);
}

/* One 1024-sample AAC frame is 1024/48000 s = 21333 µs of track; the
   position must advance exactly by that per frame. */
static void test_audio_pts_advances_with_frames(void)
{
    fprintf(stderr, "test_audio_pts_advances_with_frames\n");
    EncoderCtx *enc = open_with_audio();
    CHECK(enc != NULL, "encoder_open failed");
    if (!enc) return;

    AVFrame *f = audio_frame();
    CHECK(encoder_feed_audio(enc, f, -1) == 0, "first feed failed");
    CHECK(encoder_audio_pts_us(enc) == 21333,
          "expected 21333 us after one frame, got %lld",
          (long long)encoder_audio_pts_us(enc));

    CHECK(encoder_feed_audio(enc, f, -1) == 0, "second feed failed");
    /* 2048/48000 s rounds to nearest µs. */
    CHECK(encoder_audio_pts_us(enc) == 42667,
          "expected 42667 us after two frames, got %lld",
          (long long)encoder_audio_pts_us(enc));
    av_frame_free(&f);

    encoder_flush(enc);
    encoder_free(enc);
}

/* The first frame's PTS places the track on the session timeline exactly
   once; a later frame with a different PTS must not move it. */
static void test_audio_pts_anchors_once(void)
{
    fprintf(stderr, "test_audio_pts_anchors_once\n");
    EncoderCtx *enc = open_with_audio();
    CHECK(enc != NULL, "encoder_open failed");
    if (!enc) return;

    AVFrame *f = audio_frame();

    /* The first feed places the track 0.5 s into the session — and delivers
       its own 1024 samples, so the position lands one frame later. */
    CHECK(encoder_feed_audio(enc, f, 500000) == 0, "first feed failed");
    CHECK(encoder_audio_pts_us(enc) == 521333,
          "expected 521333 us after anchoring, got %lld",
          (long long)encoder_audio_pts_us(enc));

    /* A contradictory PTS on the second feed must be ignored: the anchor
       never moves, the track just counts samples. */
    CHECK(encoder_feed_audio(enc, f, 900000) == 0, "second feed failed");
    CHECK(encoder_audio_pts_us(enc) == 542667,
          "expected 542667 us after the second frame, got %lld",
          (long long)encoder_audio_pts_us(enc));
    av_frame_free(&f);

    encoder_flush(enc);
    encoder_free(enc);
}

/* A path that cannot be written must fail the open and hand back NULL, and
   the caller must not need to free anything. */
static void test_unwritable_path_fails_open(void)
{
    fprintf(stderr, "test_unwritable_path_fails_open\n");
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);
    EncoderCtx *enc = encoder_open("/nonexistent-dir/screencast_test.mp4",
                                   320, 240, 24, AV_PIX_FMT_BGRA,
                                   SR, &stereo, AV_SAMPLE_FMT_FLTP);
    av_channel_layout_uninit(&stereo);
    CHECK(enc == NULL, "encoder_open must return NULL on failure");
}

int main(void)
{
    make_path();
    test_open_starts_clean();
    test_no_audio_track_reports_minus_one();
    test_audio_pts_advances_with_frames();
    test_audio_pts_anchors_once();
    test_unwritable_path_fails_open();
    unlink(g_path);

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "\nall encoder tests passed\n");
    return 0;
}
