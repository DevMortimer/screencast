# Linux presenter is a captured layer surface

## Status

accepted

## Context

Linux `both` composites webcam frames inside the encoder. The finished video
contains the webcam, but the user cannot see, move, or resize it while
recording. A normal `xdg_toplevel` cannot request reliable always-on-top
placement on Wayland, and a second camera client would conflict with the
cooperative PipeWire capture policy.

## Decision

Add a switchable Linux `presenter` mode. It reuses the recorder's existing
PipeWire webcam stream and submits latest-only AVFrame references to a dedicated
presenter module. That module owns a separate Wayland connection and an
overlay-layer `wlr-layer-shell` surface.

The surface is square, borderless, rounded, and anchored with a fixed margin to
one of the captured output's four corners. A center drag selects another corner
when released. An edge or corner drag resizes the square when released, and a
scroll gesture resizes it immediately. The selected corner and size persist in
the user's XDG configuration directory.

The presenter window is part of display capture. Therefore presenter mode keeps
`MODE_DISPLAY` as the encoder's effective video input and does not run the
encoder webcam compositor. This records the window exactly once. Existing
`both` remains the encoded, non-interactive overlay.

The presenter module uses raw Wayland and the already-required libswscale. The
layer-shell and xdg-shell protocol descriptions are vendored beside the existing
screencopy protocol, so no UI toolkit or new runtime dependency is required.

## Consequences

Presenter mode requires a compositor that implements `wlr-layer-shell`; the
recording continues without the presenter when that protocol is unavailable.
The camera window is visible to the user and to display capture, so other screen
capture tools can also see it. Rendering uses bounded shared-memory buffers and
drops stale webcam frames rather than blocking the PipeWire capture thread.
