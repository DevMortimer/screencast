# Screencast

A cross-platform screencast recorder that captures a display output (plus
optional webcam and audio) and renders an MP4. Runs on Linux (wlroots Wayland
compositors) and macOS. This glossary pins down the capture, audio and timing
vocabulary so the code and docs agree.

Where a term is platform-specific it says so, and several are: Linux composites
a webcam into the picture itself and has modes for it, while macOS records the
display and leaves the presenter to the system. The two platforms deliberately
share the encoder, the mixer, and the timing vocabulary below; they share
nothing else.

## Language

### Video capture

**Display capture**:
The screen output being recorded. On Linux this is the `wlr-screencopy`
protocol; on macOS it is a ScreenCaptureKit stream. The term names the *thing
captured*, not the mechanism.
_Avoid_: screen grab, output capture

**Webcam** (Linux):
The user's camera video, captured as a PipeWire client. Distinct from display
capture; overlaid on the display in `both` mode. macOS has no equivalent term:
there the camera reaches a recording only as a presenter overlay, which this
project does not capture, composite, or position.
_Avoid_: camera video, cam, facecam

**Mode** (Linux):
What the recording currently shows: `display`, `webcam`, or `both`. Switchable
mid-recording over the control socket, which is why the output stream's
dimensions and the overlay geometry are fixed for a whole session — they cannot
change under a running encoder. macOS records the display and has no modes.
_Avoid_: layout, view, scene

**Presenter overlay** (macOS):
The system feature that segments the user from their background and composites
them into a ScreenCaptureKit stream. Offered by macOS — in Control Center,
while an app is capturing the screen and using the camera at once — and
rendered by macOS, so a recording either arrives with the presenter already in
it or does not. Nothing here decides its size, position, or whether it is on.
_Avoid_: webcam overlay, facecam, picture-in-picture

**Camera client**:
An app holding a capture device open. On macOS this is the whole reason the
camera is opened at all: the presenter overlay is offered only to an app that
is a camera client and a screen capturer simultaneously. The frames it
delivers are discarded.
_Avoid_: camera user, capture session

**Points and pixels** (macOS):
macOS reports display geometry in *points*, a resolution-independent unit;
ScreenCaptureKit wants its stream configured in *pixels*. On a Retina panel
these differ, and confusing them silently records a downscaled capture.
_Avoid_: logical/physical resolution, size

**Capture scale** (macOS):
Pixels captured per point. Bounded by two things and equal to the smaller: the
measured ratio of the display mode's pixel width to its point width, and the
**capture cap**. Not adjustable at runtime.
_Avoid_: retina factor, DPI, zoom, native resolution

**Native ratio** (macOS):
The measured pixels-per-point of the current display mode — the size of the
framebuffer the window server composites into. On a scaled HiDPI mode that
framebuffer is *larger* than the panel (1440x900 points reports 2880x1800,
which the display then resamples down to 2560x1664), so this is not the
panel's resolution. Capture never exceeds it, because asking for more pixels
than were drawn cannot add detail.
_Avoid_: native resolution, backing scale

**Capture cap** (macOS):
The ceiling on the longer edge of the captured canvas, in pixels. Exists
because the native ratio describes what the window server drew, not what is
worth recording: the pixel count multiplies the cost of every stage after it,
and beyond a point buys detail nobody watching a screencast can see.
_Avoid_: max resolution, limit, quality setting

**Zero-copy path** (macOS):
Capture to encode without the pixels leaving GPU memory: ScreenCaptureKit hands
over IOSurface-backed `CVPixelBuffer`s and VideoToolbox encodes them directly,
untouched. There is no compositing stage to pass through — a presenter, when
there is one, was composited by the system before the buffer arrived.
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

**Nearest-PTS matching** (Linux):
Pairing a webcam frame with the screen frame it was actually captured alongside,
by choosing the one whose PTS is closest rather than the one most recently
delivered. Camera pipelines run tens of milliseconds behind the display, by a
margin that moves with exposure time, so "most recent" pairs a frame with a
neighbour it was never contemporaneous with. Does not arise on macOS, where
nothing is paired: one stream arrives, already composited.
_Avoid_: latest frame, frame pairing

**Video clock**:
The source whose frames decide when an output frame is produced. Display
capture is clocked by the screen, so a static display costs nothing to record.
On Linux the webcam modes are clocked by the *camera* instead, because there
the screen going still says nothing about whether the picture is still —
clocking them off the screen freezes the overlay, and in `webcam` mode the
whole frame, for as long as the display happens not to change. macOS has only
the screen clock; when a presenter overlay is on, the system's compositing
keeps frames arriving on its own.
_Avoid_: frame rate, tick, driver
