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

// Opens the default microphone. Populates info.
// Returns NULL on failure (no mic, permission denied).
AvfMic *avf_mic_open(AvfMicInfo *info);

// Reads one audio frame (blocking). Caller owns the returned AVFrame.
// Returns NULL on error or stop.
// Output format: 48kHz stereo FLTP (planar float) — canonical format for mixer.
AVFrame *avf_mic_read(AvfMic *m);

// Stops capture and frees resources.
void avf_mic_close(AvfMic *m);

#endif
