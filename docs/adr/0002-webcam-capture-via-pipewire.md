# Capture the webcam via PipeWire, not raw V4L2

## Status

accepted

## Context

The webcam was opened as a raw V4L2 device (`avformat_open_input` on
`/dev/videoN` via the `video4linux2` demuxer). A raw V4L2 capture node is an
*exclusive* streaming open: only one consumer can stream a physical camera at a
time. So whenever a meeting app already held the camera, `screencast webcam` /
`both` silently lost the webcam and fell back to display+audio only — and vice
versa, screencast starting first would block the meeting. The user wants all
modes to work *while a meeting is live*, the way OBS does.

The camera can only be shared when a single userspace owner (PipeWire) grabs the
hardware and fans its frames out to many clients — **and every consumer,
including the meeting app, goes through that same owner**. On this project's
target (wlroots Wayland compositors, which run PipeWire universally, and where
the desktop-audio feature already assumes PipeWire/Pulse — see ADR 0001) that
owner is PipeWire.

## Decision

Capture the webcam as a **native `libpipewire` client** (a `pw_stream` consumer
on the default camera node) instead of opening the raw V4L2 device. The raw
V4L2 webcam path is **removed entirely** — this is PipeWire-only, no fallback.

- The camera is selected **automatically** (WirePlumber's default camera;
  `media.role=Camera`, autoconnect). No `/dev/videoN` path is ever hardcoded.
  `SCREENCAST_WEBCAM_DEV`, when set, is an *opt-in* override carrying a PipeWire
  target (node name/serial via `PW_KEY_TARGET_OBJECT`), not a device path.
- Pixel format is negotiated by PipeWire and mapped to an `AVPixelFormat`; the
  existing `composite`/encoder swscale path is unchanged. `SCREENCAST_CAM_FORMAT`
  is dropped. We offer the raw layouts a webcam is likely to produce — planar
  NV12/I420 plus the packed formats USB cameras commonly emit natively (YUY2,
  UYVY, and RGB/BGR variants) — so negotiation matches the sensor without
  forcing an upstream conversion. `SCREENCAST_CAM_SIZE`/`SCREENCAST_CAM_FPS`
  survive as negotiation *hints*: fps is honored per-client; resolution defers
  to the shared sensor stream and the actually-negotiated size is read back and
  scaled locally.
- The webcam remains **best-effort**: if PipeWire or a camera node is absent,
  recording continues as display+audio only, exactly as before.

Because the webcam now talks to camera hardware rather than the display server,
webcam capture is **display-server-agnostic** — it works identically under Xorg
and Wayland. Only the *screen* capture stays Wayland-bound (`wlr-screencopy`).

## Considered options

- **`pw-v4l2` LD_PRELOAD shim** — rejected: a launch-wrapper hack, not a product
  fix; requires wrapping every invocation and does nothing for users who launch
  the binary directly.
- **xdg-desktop-portal Camera portal** — rejected: adds a D-Bus permission
  handshake, and the target systems here have no working portal (no portal
  process; only gnome/gtk backends on niri). `libpipewire` connects to the
  running PipeWire directly with no portal dependency.
- **GStreamer `pipewiresrc`** — rejected: drags in the entire GStreamer stack
  for what is a single capture stream, out of step with this codebase's
  hand-rolled `libav*` style.
- **PipeWire-first with raw-V4L2 fallback** — rejected: carrying a second webcam
  backend solely to preserve the exact raw-open code that causes this bug is
  weight for a near-empty edge case (a wlroots desktop with no PipeWire).

## Consequences

- `libpipewire-0.3` (+ `libspa`) becomes a new build dependency.
- The webcam capture backend moves to its own unit (mirroring how `wlcap.c`
  isolates the screen backend); the raw-V4L2 webcam code in `capture.c` is
  deleted.
- Sharing works only when the meeting app is *also* on PipeWire. A browser that
  grabs the raw camera node directly (e.g. Firefox with
  `media.webrtc.camera.allow-pipewire` off) still blocks sharing — that is a
  configuration concern on the meeting-app side, outside this tool.
