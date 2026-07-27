# macOS records the display and leaves the presenter to the system

## Status

accepted

Supersedes parts of [0003](0003-cross-platform-macos-support.md): the webcam
row of its backend table, its mode-switching UX, and its reasoning that the
control-socket model required macOS to keep the same commands as Linux.

## Context

The macOS build had the same three modes as Linux — `display`, `webcam`,
`both` — implemented with a Metal compositor that drew the camera into the
captured frame, a camera frame ring, nearest-PTS pairing to match a camera
frame against the screen frame captured beside it, and a record loop that
switched between two clocks depending on the mode.

That machinery was the expensive part of the program. The webcam modes cannot
be clocked by the screen, because a still display says nothing about whether a
face is still, so they were clocked by the camera: one output frame per camera
frame, which means encoding the full canvas thirty times a second for the
length of the recording no matter what is happening. On a fanless laptop that
is the difference between a recording that keeps up and one that drops frames.

macOS 14 does this itself. Presenter Overlay segments the user from their
background and composites them into a ScreenCaptureKit stream, offered from
Control Center's Video Effects menu to any app that is capturing the screen
and using the camera at the same time. Once it is on, the frames arriving on
the stream the app already runs have the presenter in them.

So the compositor was reimplementing, worse, something the platform does on
paths an application cannot reach — and charging the whole recording for it
whether or not anyone was on camera.

## Decision

macOS records the display, and nothing else.

- The Metal compositor, its probe, the camera and screen rings, nearest-PTS
  pairing and the dual-clock record loop are deleted. Recording is clocked by
  the screen, so a static display encodes nothing.
- `webcam` and `both` are rejected on macOS in `control_parse_mode` with a
  message naming Presenter Overlay. The command vocabulary is `screencast`,
  `screencast presenter`, `screencast stop`.
- The camera is still opened — Presenter Overlay is offered only to an app that
  is a camera client and a screen capturer at once — but only for `screencast
  presenter`, and at the smallest stream the device offers, since every frame
  delivered is released on arrival.
- The captured canvas is capped at 1920 on its longer edge, and the encoder
  uses VideoToolbox constant quality.
- The macOS tuning environment variables are gone. Both capture bounds are now
  measured or fixed.

Linux keeps all three modes, its compositor, and its environment variables.
The platforms now diverge in what they record, not only in how.

## Considered options

- **Keep the modes, make the pipeline faster** — rejected. Moving the encode
  off the capture thread would have smoothed the stall without reducing the
  work; the recording would still encode a full canvas continuously to draw an
  overlay the system draws better.
- **Delete the camera entirely** — rejected. Without a camera client the Video
  Effects menu never offers Presenter Overlay, so this would have discarded the
  feature rather than delegated it.
- **Keep the modes as a fallback for macOS 13 and Intel** — rejected. It
  retains every line the change exists to delete, for hardware not on hand to
  test with.

## Consequences

- ~950 lines deleted from the macOS path.
- A still screen costs almost nothing to record again. With the overlay on,
  frames arrive continuously — the presenter is always moving — so that saving
  applies only while it is off. `outputVideoEffectDidStart` is logged under
  `SCREENCAST_DEBUG` to distinguish the two cases after the fact.
- Presenter Overlay requires macOS 14 on Apple silicon. On macOS 13 or Intel,
  screencast records the display and there is no way to appear in it.
- Whether the presenter appears, where, and at what size are no longer this
  program's to decide, and cannot be scripted or bound to a key. They are
  Control Center's.
- The webcam is no longer switchable mid-recording. It is a Control Center
  toggle instead, which is switchable mid-recording in a way the old mode
  hotkeys were — this is a change of interface, not a loss of capability.

## Open questions

Whether requesting the smallest camera format constrains what Presenter
Overlay renders is unverified. The documentation says ScreenCaptureKit "will
take the camera" when the overlay engages, which reads as the system
reconfiguring the device for its own use, but it does not say so outright. The
camera is currently opened at 320x240. If the presenter looks soft, the
fallback is to request 1920x1080 as before — the cost is power for pixels
nothing reads, not correctness.

Note also that the output's buffer dimensions, not the session preset and not
the device's `activeFormat`, are what actually decide the delivered size on
macOS. A session re-applies its preset on `startRunning` and overwrites any
format set beforehand, and `AVCaptureSessionPresetInputPriority` — which would
say "defer to the device" — is iOS-only.
