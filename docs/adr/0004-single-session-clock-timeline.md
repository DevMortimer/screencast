# A single session clock stamps every track at capture time

## Status

accepted

## Context

Recordings drifted out of sync, and the drift got worse the harder the machine
was working. On a fanless laptop half an hour in, audio and video were audibly
apart.

The cause was where the timestamp came from. Video PTS was stamped when a frame
reached the encoder, behind a queue. That makes the timestamp a measure of how
busy the CPU was, not of when the frame was captured: whenever encoding fell
behind, frames were stamped late, and the timeline silently absorbed the error.
Nothing reported a failure — the recording simply came out wrong.

The same class of bug existed on every other track. Each source started at a
different moment (the screen stream must already be running before the recorder
can confirm a first frame; the webcam and microphone take further hundreds of
milliseconds), and each was counting from its own start, so every track entered
the recording with a different head start.

Audio made it worse in a way video does not. An audio track's PTS is a running
sample count, so samples a device fails to deliver are not heard as a dropout —
they are heard as every later sample arriving early, permanently. The mixer's
bounded FIFOs did exactly this: when one source stalled, the other's samples
piled up until the oldest were discarded to make room.

## Decision

Every timestamp in a recording derives from one monotonic session clock, read
from the capture buffer rather than from the clock at the moment the encoder
reaches it.

On macOS that clock is `CMClockGetHostTimeClock()` — the clock ScreenCaptureKit
and AVFoundation already stamp their sample buffers with, so no conversion or
correlation is needed. The session is anchored once, after the last source is
live; everything captured before that anchor is startup and is discarded.

Three rules follow from it, and they are ordered:

1. **A/V sync is inviolable.** Everything else may degrade to preserve it.
2. **Video frames are the elastic resource.** Under load, drop frames and let
   variable frame rate carry it. A dropped frame means the previous one displays
   slightly longer, which is correct in a VFR MP4. Never re-stamp a timestamp to
   make a queue look healthy.
3. **Audio is never dropped.** A gap in a source becomes silence in the track,
   inserted at the point the samples were missing, so the gap is *in* the
   timeline rather than shifting it.

Rule 3 is why `mixer_feed` carries a capture timestamp: it is what distinguishes
a source that skipped samples from one that merely delivered them late.

Only a source's own timestamps may move it on the timeline. This is the sharp
edge of the rule and it was learned the hard way: a first attempt also padded
any source whose buffer looked shallow beside the others, which reads plausibly
and is wrong. Buffer depth is not a position on the timeline — silence inserted
for a gap makes a buffer deeper without any time having passed — so that rule
took one source's gap and injected it into a healthy source as well, shifting
the whole mix by exactly the amount it was trying to repair.

A stalled source instead simply holds the lockstep minimum at zero while the
live source buffers. That is why the buffer bound is sized to outlast the stall
timeout rather than set to a comfortable jitter window: nothing may be discarded
before the timeout has had its chance to resolve the stall.

## Consequences

Recordings hold sync end to end regardless of load: a 20 s capture measures
18.133 s of video against 18.120 s of audio, both starting at zero, with the
webcam visibly matched to the screen frame it was captured with.

Thermal behaviour improved as a side effect rather than as a separate piece of
work. The two problems were the same problem — CPU saturation was corrupting the
timeline — so the fix that stopped the corruption is also what let the pipeline
shed load safely.

The costs:

- **Every capture backend must expose a capture timestamp.** Backends that have
  none pass `AV_NOPTS_VALUE` and fall back to counting samples, which drifts if
  that source ever loses data. This is the weaker path and should shrink.
- **Startup discards real capture.** Buffers arriving before the anchor are
  thrown away, so the recording begins a fraction of a second after the command
  does. This is the intended trade: a late start is invisible, a head start is
  not.
- **Silence padding can mask a genuinely broken source.** A device delivering
  nothing produces a valid recording full of silence rather than an error. The
  mixer logs each insertion under `SCREENCAST_DEBUG`, and drops a source
  outright after two seconds, but the recording itself will not fail.
