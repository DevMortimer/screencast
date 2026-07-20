# Screencast

A Wayland screencast recorder that captures a display output (plus optional
webcam and audio) and renders an MP4. This glossary pins down the audio and
capture vocabulary so the code and docs agree.

## Language

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
