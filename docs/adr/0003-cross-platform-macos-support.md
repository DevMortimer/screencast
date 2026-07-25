# Cross-platform macOS support with platform-native capture backends

## Status

accepted

## Context

The tool was Linux-only: `wlr-screencopy` for display, PipeWire for webcam,
PulseAudio/ALSA for audio, and NVIDIA NVENC for encoding. None of these exist
on macOS.

The goal is the same CLI, daemon, control-socket, and keybind workflow on
macOS, with Linux support preserved.

## Decision

Add a macOS target with platform-native capture backends. The codebase splits
into shared modules (`src/`) and platform-specific modules (`src/linux/`,
`src/macos/`). A single Makefile auto-detects the platform via `uname`.

| Layer | Linux | macOS |
|---|---|---|
| Screen capture | `wlr-screencopy` | ScreenCaptureKit |
| Desktop audio | PulseAudio monitor source | ScreenCaptureKit (bundled) |
| Webcam | PipeWire client | AVFoundation |
| Microphone | PulseAudio/ALSA | AVFoundation/CoreAudio |
| Camera arbitration | Custom `arbiter.c` | None (AVFoundation natively fans out) |
| Encoder | `h264_nvenc` | `h264_videotoolbox` |
| Encode pipeline | Two-pass (capture + final render) | Single-pass |
| Keybinds | Compositor (niri/Sway/etc.) | `skhd` |
| Notifications | `notify-send` | `osascript display notification` |

macOS deviates deliberately where the platform makes it natural:
- No two-pass encode — VideoToolbox has no NVENC-style presets; the user can
  post-process manually.
- No in-video recording indicator — macOS enforces a system indicator.
- No camera arbitration — AVFoundation handles multiple consumers natively.
- No display override — the daemon locks to the display it was invoked from.
- Single `~/Movies/` output directory instead of `$HOME/`.
- Control socket at `~/Library/Caches/screencast/screencast.sock`.

Env vars that have no macOS equivalent (`SCREENCAST_OUTPUT`, `SCREENCAST_DESKTOP_DEV`,
`SCREENCAST_WEBCAM_DEV`, all NVENC tuning vars, `SCREENCAST_KEEP_CAPTURE`) are
silently ignored on macOS.

## Considered options

- **Cross-platform capture library (SDL, libavdevice)** — rejected. No single
  library exposes screen capture, webcam, system audio, and hardware encoding
  with the control this tool requires. ScreenCaptureKit and PipeWire have no
  common abstraction.
- **`#ifdef` within existing files** — rejected. The capture paths share no
  code. One `#ifdef` per platform call in `main.c` would become unreadable.
- **Separate macOS binary (no daemon)** — rejected. The daemon/control-socket
  model is the core UX; dropping it for macOS would split the project in two.

## Consequences

- `src/` is reorganized into `src/linux/` and `src/macos/`.
- The Linux build gains no new dependencies; NVENC remains a hard requirement.
- The macOS build requires clang and links against AVFoundation,
  ScreenCaptureKit, CoreMedia, and CoreAudio frameworks.
- macOS requires `skhd` for keybinds (documented in README).
- Camera arbitration (`arbiter.c`, its tests) is Linux-only.
