# Screencast

A cross-platform screencast recorder that captures a display output (plus
optional webcam and audio) and renders an MP4. Runs on Linux (wlroots Wayland
compositors) and macOS. This glossary pins down the capture, audio and timing
vocabulary so the code and docs agree.

Where a term is platform-specific it says so. The two platforms deliberately
share the encoder, the mixer, and the timing vocabulary below; they share
nothing else.

## Language

### Video capture

**Display capture**:
The screen output being recorded. On Linux this is the `wlr-screencopy`
protocol; on macOS it is a ScreenCaptureKit stream. The term names the *thing
captured*, not the mechanism.
_Avoid_: screen grab, output capture

**Webcam**:
The user's camera video — a PipeWire client on Linux, an AVFoundation capture
device on macOS. Distinct from display capture; overlaid on the display in
`both` mode.
_Avoid_: camera video, cam, facecam

**Mode**:
What the recording currently shows: `display`, `webcam`, or `both`. Switchable
mid-recording over the control socket, which is why the output stream's
dimensions and the overlay geometry are fixed for a whole session — they cannot
change under a running encoder.
_Avoid_: layout, view, scene

**Points and pixels** (macOS):
macOS reports display geometry in *points*, a resolution-independent unit;
ScreenCaptureKit wants its stream configured in *pixels*. On a Retina panel
these differ, and confusing them silently records a downscaled capture.
_Avoid_: logical/physical resolution, size

**Capture scale** (macOS):
Pixels captured per point. Defaults to the display's measured backing-store
ratio, which on the scaled HiDPI modes people actually run is not a round 2 —
1440x900 points on a 2560x1600 panel is 1.78. Overridable with
`SCREENCAST_SCALE`, clamped so it never upscales.
_Avoid_: retina factor, DPI, zoom

**Zero-copy path** (macOS):
Capture to encode without the pixels leaving GPU memory: ScreenCaptureKit and
AVFoundation hand over IOSurface-backed `CVPixelBuffer`s, the Metal compositor
renders into another, and VideoToolbox encodes that one directly.
_Avoid_: hardware path, GPU pipeline

**Camera node**:
The PipeWire graph node representing the shared camera source that screencast
captures from. Selected automatically as the system default unless overridden.
_Avoid_: /dev/video, V4L2 device, camera device file

**Fan-out**:
PipeWire owning one physical camera and serving its frames to multiple
simultaneous consumers (e.g. a meeting app and screencast at once). Only works
when every consumer goes through PipeWire.
_Avoid_: camera sharing, multiplexing, exclusive access

### Audio

**Source**:
A capture device that audio is read *from* (a microphone, or the monitor of a
sink). PulseAudio/PipeWire concept.

**Sink**:
A playback device that audio is written *to* (speakers, headphones).

**Monitor source**:
The virtual source attached to every sink that carries exactly what the sink is
playing. Capturing it yields desktop audio.
_Avoid_: loopback

**Microphone audio**:
Audio captured from the default input source (the user's voice).
_Avoid_: mic input, recording device

**Desktop audio**:
Audio captured from the default sink's monitor source (application/system
sound: video calls, music, game audio).
_Avoid_: system audio, loopback audio, internal audio

**Mixed audio**:
The single output audio track produced by combining microphone audio and
desktop audio into one stream.

**Lockstep**:
The rule that keeps the two audio sources aligned: on every feed the mixer emits
only `min(available)` samples across the live sources, so neither can run ahead
of the other.
_Avoid_: sync, alignment

**Primed**:
A source that has actually delivered at least one sample. Only primed sources
count toward the lockstep minimum — a source that is configured but never
materialises would otherwise pin that minimum at zero and suppress the whole
track.
_Avoid_: ready, active, connected

**Silence padding**:
Writing silence into a source's buffer to stand in for samples it never
delivered, so the gap is *in* the track rather than shifting everything after
it. See "Elastic video" for why audio is never dropped instead.
_Avoid_: filling, concealment, interpolation

### Timing

**Session clock**:
The single monotonic clock every timestamp in a recording derives from. On macOS
it is `CMClockGetHostTimeClock()`, the clock ScreenCaptureKit and AVFoundation
already stamp their buffers with, so screen, webcam, microphone and desktop
audio land on one timeline no matter how long each took to start.
_Avoid_: wall clock, system time, reference clock

**Session anchor** (`t0`):
The session-clock reading that PTS 0 maps to, fixed once after the last source
is live. Everything captured before it is startup, not recording, and is
discarded — this is what stops a source that started early from entering with a
head start on the others.
_Avoid_: start time, epoch, zero point

**Presentation timestamp** (PTS):
When a frame or sample was *captured*, relative to the session anchor — read
from the capture buffer, never from the clock at the moment the encoder happens
to reach it. Stamping at encode time is what let CPU saturation silently corrupt
the timeline; a backlogged queue must delay delivery without moving a timestamp.
_Avoid_: frame time, capture time (ambiguous)

**Elastic video**:
The priority rule the whole pipeline is built around: A/V sync is inviolable,
and video frames are the resource that gives under load. Drop frames and let
variable frame rate carry it; never re-stamp a timestamp, never drop audio.
_Avoid_: frame skipping, throttling

**Nearest-PTS matching**:
Pairing a webcam frame with the screen frame it was actually captured alongside,
by choosing the camera frame whose PTS is closest. Taking "the most recent
camera frame" instead pairs the screen with whatever the camera pipeline had got
around to delivering, which runs tens of milliseconds behind.
_Avoid_: latest frame, frame pairing
