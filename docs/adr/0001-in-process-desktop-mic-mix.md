# In-process mix of desktop and microphone audio

## Status

accepted

## Context

The recorder captured only microphone audio (the default PulseAudio source);
desktop audio (the default sink's monitor source) was never captured, in either
the old X11 build or the current Wayland one. We want a single output audio
track containing both the user's voice and application/system sound.

## Decision

Capture microphone audio and desktop audio as two independent sources and mix
them into the single existing audio stream **in-process, by hand**:

- Each source is resampled (`swr`) to a canonical format (48 kHz stereo FLTP)
  into its own `AVAudioFifo`, then a mix stage sums the two at **unity gain**
  and hard-clamps the result to [-1, 1] before feeding the existing encoder
  path.
- Desktop audio comes from the Pulse device `@DEFAULT_MONITOR@` (the default
  sink's monitor), so it tracks the default sink and needs no `pactl` query.
- The mix runs **lockstep on the minimum** of the two FIFOs' available samples,
  and each FIFO is **bounded** (~200 ms); on overrun the oldest samples of the
  faster source are dropped.

## Considered options

- **libavfilter `amix`** — rejected: pulls in a new library/link dependency and
  a filter-graph lifecycle for what is a two-input sum, out of step with the
  codebase's existing hand-rolled `swr` + `AVAudioFifo` audio path.
- **Delegate mixing to PulseAudio** (combined/null sink via runtime modules) —
  rejected: mutates the user's live audio configuration, needs teardown, and is
  fragile across Pulse/PipeWire setups.
- **Per-source gain / a limiter** — rejected: mic-vs-desktop balance is
  controlled at the OS mixer (per-app and master volume), so a second gain stage
  in the recorder is redundant. Unity + clamp keeps the live path simple.
- **Unbounded or timestamp-driven (PTS) drift correction** — rejected:
  unbounded FIFOs can grow without limit over long (10 min+) recordings; full
  PTS resync is more bookkeeping than desktop+mic alignment warrants. Bounded
  FIFOs keep both memory and alignment in check.

## Consequences

- Desktop audio requires PulseAudio/PipeWire. Under the pure-ALSA mic fallback,
  there is no monitor source, so recordings are mic-only.
- Audio is **best-effort**: whichever sources open are mixed; if only one opens,
  it is recorded alone; if both fail, the recording is video-only. The set of
  live sources is reported (stderr / `notify-send`).
- Desktop capture is **on by default**; `SCREENCAST_DESKTOP_AUDIO=0` disables it
  and `SCREENCAST_DESKTOP_DEV` overrides the monitor device name.
- `encoder_open` is initialized with the canonical mixed format rather than the
  raw microphone format.
