// avf_mic.h — AVFoundation / CoreAudio microphone capture
#ifndef AVF_MIC_H
#define AVF_MIC_H

#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

typedef struct AvfMic AvfMic; // opaque

typedef struct {
    int sample_rate;
    AVChannelLayout ch_layout;
    enum AVSampleFormat sample_fmt;
} AvfMicInfo;

/* Opens the selected microphone, or the system default when target is NULL or
 * empty. `target` may be an AVCaptureDevice unique ID or name substring. */
AvfMic *avf_mic_open(const char *target, AvfMicInfo *info);

// Anchors the session timeline and drops everything captured before it.
// The microphone is opened during recording setup and starts delivering
// immediately, so without this the samples buffered while the encoder was
// still being built enter the recording as its first audio.
void avf_mic_start_session(AvfMic *m, int64_t t0_us);

// Reads one audio frame (blocking). Caller owns the returned AVFrame.
// Returns NULL on error or stop.
// Output format: 48kHz stereo FLTP (planar float) — canonical format for mixer.
// *pts_us receives the session-relative timestamp of the frame's first sample.
AVFrame *avf_mic_read(AvfMic *m, int64_t *pts_us);

// Stops capture and frees resources.
void avf_mic_close(AvfMic *m);

#endif
