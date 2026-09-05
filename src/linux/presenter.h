#pragma once
#include <libavutil/frame.h>

/*
 * Borderless Linux presenter window.
 *
 * The implementation owns its Wayland connection and rendering thread.  It
 * uses layer-shell to stay above normal windows and accepts webcam AVFrames at
 * this small seam.  Frames are latest-only: submission never blocks capture.
 */
typedef struct Presenter Presenter;

/*
 * Open a square presenter on output_name (NULL lets the compositor choose).
 * Returns after the Wayland surface is ready, or NULL when layer-shell is not
 * available.  The saved corner and size are restored automatically.
 */
Presenter *presenter_open(const char *output_name);

/* Retain and schedule one webcam frame.  Safe on the PipeWire capture thread. */
void presenter_submit(Presenter *presenter, const AVFrame *frame);

/* Stop the rendering thread, close the Wayland surface, and save preferences. */
void presenter_close(Presenter *presenter);
