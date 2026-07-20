# Screencast

A Wayland screencast recorder that captures a display output (plus optional
webcam and audio) and renders an MP4. This glossary pins down the audio and
capture vocabulary so the code and docs agree.

## Language

### Video capture

**Display capture**:
The screen output recorded via the `wlr-screencopy` protocol. This is the only
capture path bound to Wayland.
_Avoid_: screen grab, output capture

**Webcam**:
The user's camera video, captured as a PipeWire client from the camera node.
Distinct from display capture; overlaid on the display in `both` mode.
_Avoid_: camera video, cam, facecam

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
