# macOS recording yields to the system

## Status

accepted

Supersedes parts of [0005](0005-macos-records-the-display-only.md): the
deletion of macOS tuning variables, the fixed 1920 capture cap, and the
fixed 30 fps.

## Context

Recording while the machine was busy — a video call sharing the screen, a
Next.js dev server, a Flutter toolchain — froze the whole system for about a
second when a heavy burst arrived (opening the iOS simulator). The finished
recording was fine; the machine was not. The recorder's standing load — a
1920-long-edge capture encoded at 30 fps, continuously, because a shared
screen always changes — had consumed the headroom, and the burst saturated
the GPU and memory. On a machine that also encodes the same screen for the
call itself, every pixel is paid for twice.

This is saturation, not a throttle: a momentary freeze when a shared resource
hits 100%, not a sustained slowdown. The fix is a budget, not a hero change:
cut the standing load, bound the bursts, keep the timeline honest.

## Decision

macOS recording is tuned to leave the system headroom, and the knobs to
adjust it are back.

- **Capture cap: 1440 on the longer edge, by default.** ~40% of the pixels of
  the old 1920 cap, still oversampling a 1440x900-point desktop. Dialled with
  `SCREENCAST_CAPTURE_CAP` (640-4096).
- **Frame rate: 24 fps, by default.** A talking head and a shared screen read
  perfectly at 24; every frame saved is GPU time the rest of the system
  keeps. Dialled with `SCREENCAST_FPS` (10-60).
- **Encoder power path: full, by default.** `power_efficient` is off;
  `SCREENCAST_VT_POWER_EFFICIENT=1` restores the low-power path. The full
  encoder finishes each frame sooner, freeing the GPU while the system is
  under load. The low-power path stays available for battery sessions when
  nothing is competing for the GPU.
- **Constant quality stays a knob:** `SCREENCAST_VT_QUALITY` (1-100, default
  65). Quality is bits, and bits are encode time.
- **The capture queue is shallow (8 frames, ~270 ms) and drops the oldest.**
  A backlog is dropped, never drained: encoding queued frames back-to-back
  after a stall would add a GPU burst at exactly the moment the system is
  recovering. A warning is printed once per backlog episode so the log says
  when the system was under load.
- **Desktop audio buffering is bounded (2 s); newest samples are dropped.**
  Unbounded accumulation under a downstream stall would balloon RAM on a
  machine already under memory pressure. The timeline stays anchored at the
  head and the mixer covers the hole with silence.
- The session timeline, elastic video (drop frames, never re-stamp, never
  drop audio), zero-copy NV12, and single-pass encode are all unchanged.

## Considered options

- **Pipeline-only restructure (encode thread, async writer)** — rejected for
  now. It hides stalls without reducing the work, and the symptom was a
  system freeze under load, not a broken recording. The shallow queue and
  bounded buffers achieve the yield with less machinery. An encode or write
  thread remains the fallback if stalls ever surface in the recording.
- **Keep 1920/30 and fight harder (`power_efficient=0` alone)** — rejected.
  One knob cannot buy back 60% of the pixels.
- **No knobs at all** — rejected. 0005's fixed values were chosen when a
  fanless laptop recording a still screen was the only cost model. Under a
  shared, loaded machine, a recording that cannot yield is a liability.

## Consequences

- The default recording is softer than before: 1440 wide at 24 fps instead of
  1920 at 30. Viewers of a screencast see text, not pixel counts.
- ADR 0005's "no tuning variables" stance is partially reversed; the four
  variables above are the whole surface.
- The backlog warning gives the log a line that says "the system was under
  load here" without needing `SCREENCAST_DEBUG`.
- Linux is untouched: its environment variables, modes, and pipeline are
  unchanged.
