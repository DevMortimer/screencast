# Screencast

A small Wayland screencast recorder written in C. It records an output via the
`wlr-screencopy` protocol, optional webcam video, and audio (microphone plus
desktop audio, mixed), then renders a final MP4 with NVIDIA NVENC.

It targets wlroots-based compositors (niri, Sway, Hyprland, river, …) that
implement `wlr-screencopy-unstable-v1`.

**Platforms:** Linux (wlroots Wayland) | macOS 13+

The two platforms record different things. Linux composites a webcam into the
picture itself and has modes for it; macOS records the display and leaves the
presenter to the system's Presenter Overlay. The features below describe the
Linux build — see [macOS](#macos) for that one.

## Features (Linux)

- Records a Wayland output via `wlr-screencopy` (shm buffers).
- Supports display-only, webcam-only, and display-plus-webcam modes. (macOS records the display only.)
- Captures the webcam as a **PipeWire** client. The webcam path is
  display-server-agnostic and works under both Xorg and Wayland.
- **Cooperative camera capture.** `display` recording never touches the webcam
  or the camera node, so it can never disrupt a meeting that is using the
  camera. `webcam`/`both` engage the webcam only when the camera node is free;
  if another app already holds it, screencast declines gracefully — it keeps
  recording the display and notifies you — and engages the webcam automatically
  once the camera frees. Switching back to `display` hands the camera back.
- Captures microphone audio (PulseAudio/PipeWire `default`, ALSA fallback) and
  desktop audio (the default sink's monitor), mixed into a single track. Every
  source is best-effort: whatever is available is recorded, and a source that
  goes away mid-recording is dropped cleanly so the recording continues.
- Burns a small red recording indicator into the top-right of the video.
- Writes a high-quality intermediate MP4 and transcodes it to a final
  `h264_nvenc` MP4.

## Requirements

- A wlroots-based Wayland compositor with `wlr-screencopy-unstable-v1`.
- GCC and `make`.
- `wayland-scanner` and the `wayland-client` library.
- FFmpeg development libraries:
  - `libavformat`
  - `libavcodec`
  - `libavdevice`
  - `libswscale`
  - `libswresample`
  - `libavutil`
- `libpipewire-0.3` (the webcam is captured as a PipeWire client).
- FFmpeg CLI available as `ffmpeg`.
- Desktop notifications via `notify-send`.
- NVIDIA GPU/driver stack with `h264_nvenc` support.
- A running PipeWire server is needed for the webcam (and for desktop audio).

On Debian/Ubuntu-based systems, the packages are typically:

```sh
sudo apt install build-essential pkg-config ffmpeg \
  libavformat-dev libavcodec-dev libavdevice-dev libswscale-dev \
  libswresample-dev libavutil-dev libwayland-dev wayland-protocols \
  libpipewire-0.3-dev libnotify-bin
```

The `wlr-screencopy-unstable-v1.xml` protocol is vendored under `protocols/`,
so `wlr-protocols` is not required to build.

## Build

```sh
make
```

The compiled binary is written to `./screencast`.

To remove build artifacts:

```sh
make clean
```

## macOS

On macOS, screencast uses native frameworks:
- **ScreenCaptureKit** for display capture and system audio
- **AVFoundation** for the microphone
- **VideoToolbox** for hardware-accelerated H.264 encoding, at constant quality

Frames never leave GPU memory between capture and encode.

### Requirements (macOS)

- macOS 13 (Ventura) or later; macOS 14 (Sonoma) on Apple silicon for Presenter Overlay
- Xcode Command Line Tools (`xcode-select --install`)
- FFmpeg development libraries:
  - `libavformat`, `libavcodec`, `libavdevice`, `libswscale`, `libswresample`, `libavutil`
- FFmpeg CLI available as `ffmpeg` (for the shared encoder)

Install FFmpeg via Homebrew:

```sh
brew install ffmpeg
```

### Build (macOS)

```sh
make          # ./screencast and, via `make bundle`, ./Screencast.app
make install  # ~/.local/bin/screencast + ~/Applications/Screencast.app
```

The compiled binary is written to `./screencast`. `make install` also builds
`Screencast.app`, a minimal app bundle around the same binary — `presenter`
needs it (below). The bundle is ad-hoc signed, so macOS asks for the
privacy permissions again after a rebuild; pass `CODESIGN_ID=<identity>` to
sign with a stable certificate instead.

### macOS usage

macOS records the display. There are two commands:

```sh
screencast            # record the display + microphone + desktop audio
screencast presenter  # the same, but make Presenter Overlay available
screencast stop
```

**To appear in the recording**, run `screencast presenter` and turn on
Presenter Overlay from the Video Effects menu in Control Center. macOS
segments you from your background and composites you into the capture
itself — you can switch it on and off, and between the small and large
layouts, as often as you like while recording.

The `presenter` form exists because that menu item only appears for an app
using the camera and the screen at once. Plain `screencast` opens no camera,
so it costs no power and lights no camera indicator.

`presenter` hands the recording off to `~/Applications/Screencast.app`
(via LaunchServices) rather than recording in the calling process. Control
Center attributes a camera stream to the responsible process, and the Video
Effects panel only lists processes with an app identity — launched bare from
skhd or a zellij server there is no app on the chain, so the camera runs but
the overlay is unreachable. The bundle gives every launcher the same
identity. The first bundled run asks for Camera, Microphone, and Screen
Recording permissions under the name **Screencast**.

Bind them with **skhd** (or any macOS hotkey tool):

```cfg
shift + cmd - s : pgrep -x screencast > /dev/null || \
                  ("$HOME/.local/bin/screencast" >> "$HOME/Library/Logs/screencast.log" 2>&1 &)
shift + cmd - p : pgrep -x screencast > /dev/null || \
                  ("$HOME/.local/bin/screencast" presenter >> "$HOME/Library/Logs/screencast.log" 2>&1 &)
cmd - escape    : pgrep -x screencast > /dev/null && "$HOME/.local/bin/screencast" stop
```

Launched from a hotkey there is no terminal, so the daemon's output goes to a
log — that is where a failed start can be read back from. The bundled daemon
appends to the same log itself.

### macOS paths

| Item | Path |
|---|---|
| Output recordings | `~/Movies/screencast_YYYYMMDD_HHMMSS.mp4` |
| Control socket | `~/Library/Caches/screencast/screencast.sock` |
| App bundle (presenter identity) | `~/Applications/Screencast.app` |
| Daemon log | `~/Library/Logs/screencast.log` |

### macOS limitations vs Linux

- **No webcam modes.** Linux has `display`, `webcam` and `both`, and composites the camera itself. macOS records the display and leaves the presenter to the system — see [ADR 0005](docs/adr/0005-macos-records-the-display-only.md). Sending `webcam` or `both` to a macOS build is rejected with a pointer to Control Center.
- **Presenter Overlay needs macOS 14 on Apple silicon.** On macOS 13 or Intel, screencast records the display and there is no way to appear in it.
- **Presenter Overlay is not scriptable.** It is a Control Center toggle. Nothing can bind it to a key or turn it on from the command line.
- **Capture is capped at 1920 on the long edge**, below the full backing store on a HiDPI panel. The pixel count multiplies the cost of every stage after it, and beyond that point buys detail nobody watching a screencast can see.
- **Single-pass encode.** No two-pass NVENC render; VideoToolbox encodes directly to the final file at constant quality.
- **No recording indicator.** macOS enforces its own system recording indicator in the menu bar.
- **No display override.** The daemon locks to the display the cursor was on when it started.
- **No tuning variables.** Every `SCREENCAST_*` variable except `SCREENCAST_DEBUG` is Linux-only. There is no intermediate capture file to keep.

## Usage (Linux)

Wayland compositors own global keybindings, so — unlike the old X11 version —
`screencast` no longer grabs hotkeys itself. Instead it is a small
daemon/controller:

```sh
screencast display   # start recording the screen + audio (becomes a daemon)
screencast webcam    # switch the running recorder to webcam + audio
screencast both      # switch to screen + webcam overlay + audio
screencast stop      # stop and render the final MP4
```

The first record command starts a background daemon and begins recording. Later
invocations reach that daemon over a control socket
(`$XDG_RUNTIME_DIR/screencast.sock`) and switch its mode live within the same
file. `screencast stop` ends the recording and kicks off the final render.

Bind these to compositor keys. For **niri** (`config.kdl`), using the original
screencast shortcuts:

```kdl
binds {
    Mod+Shift+D { spawn "screencast" "display"; }
    Mod+Shift+W { spawn "screencast" "webcam"; }
    Mod+Shift+B { spawn "screencast" "both"; }
    Mod+Escape  { spawn "screencast" "stop"; }
}
```

Recordings are written to the home directory:

- Intermediate capture: `~/screencast_YYYYMMDD_HHMMSS_capture.mp4`
- Final output: `~/screencast_YYYYMMDD_HHMMSS.mp4`

The intermediate file is removed after a successful final render unless
`SCREENCAST_KEEP_CAPTURE` is set.

## Configuration (Linux)

The recorder can be tuned with environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `SCREENCAST_OUTPUT` | focused output | Wayland output name to capture (e.g. `DP-1`, `HDMI-A-1`). Match `wlr-randr`/`niri msg outputs` names. When unset, the focused output is used on niri (`niri msg focused-output`), else the first output. |
| `SCREENCAST_DRAW_MOUSE` | `1` | Composite the cursor into the recording; set to `0` to hide it. |
| `SCREENCAST_DESKTOP_AUDIO` | `1` | Mix desktop audio (default sink's monitor) into the track; set to `0` to record microphone only. |
| `SCREENCAST_DESKTOP_DEV` | `@DEFAULT_MONITOR@` | PulseAudio/PipeWire source for desktop audio. Override with a concrete monitor name from `pactl list sources` (e.g. `alsa_output.<…>.monitor`). |
| `SCREENCAST_WEBCAM_DEV` | `auto` | PipeWire camera target (node name or serial), or `auto` for the system default camera. Not a `/dev/video*` path. |
| `SCREENCAST_CAM_FPS` | `30` | Preferred webcam frame rate (a PipeWire negotiation hint). |
| `SCREENCAST_CAM_SIZE` | `640x360` | Preferred webcam capture size. Kept intentionally low by default — the corner overlay is ≤480 px and higher resolutions burn CPU/GPU on frames that get scaled down, which can push audio out of sync. Raise it if you need more webcam detail. |
| `SCREENCAST_NVENC_CAPTURE_PRESET` | `p3` | NVENC preset for the real-time intermediate capture. |
| `SCREENCAST_NVENC_CAPTURE_QP` | `12` | Constant QP for the intermediate capture. |
| `SCREENCAST_NVENC_FINAL_PRESET` | `p7` | NVENC preset for the final render. |
| `SCREENCAST_NVENC_FINAL_CQ` | `16` | Constant quality value for the final render. |
| `SCREENCAST_NVENC_FINAL_LOOKAHEAD` | `32` | NVENC final render lookahead. |
| `SCREENCAST_NVENC_FINAL_AQ` | `10` | NVENC adaptive quantization strength. |
| `SCREENCAST_KEEP_CAPTURE` | unset | Keep the intermediate capture file when set to any non-empty value. |

> **Note:** every variable in the table above is Linux-only.

### macOS configuration

There isn't any. macOS reads no configuration variables except
`SCREENCAST_DEBUG`, which reports rather than tunes: per-frame delivery
counts, a periodic A/V sync line, and whether Presenter Overlay is on.

Capture resolution, encoder quality and camera format are all fixed —
measured from the display, or chosen once. If one of them is wrong on your
hardware that is a bug worth fixing rather than a value worth overriding.

Example:

```sh
SCREENCAST_OUTPUT=DP-1 SCREENCAST_KEEP_CAPTURE=1 screencast display
```

## Notes

Screen capture uses `wlr-screencopy-unstable-v1` with `wl_shm` buffers. The
webcam is captured as a PipeWire client (`libpipewire`) — see
`docs/adr/0002-webcam-capture-via-pipewire.md`. Because true fan-out (one camera
serving several consumers) is not available on all hardware, screencast is a
*cooperative* consumer of the camera node rather than a grabber: it acquires the
node only for `webcam`/`both`, declines instead of fighting when the node is
busy, and releases it when it returns to `display`. The audio paths use FFmpeg's
libavdevice.

Microphone and desktop audio are captured as two independent sources, each
resampled to a canonical 48 kHz stereo format and summed (unity gain, hard
clamp) into one track. Mixing is lockstep with bounded per-source buffers, so
the two streams stay aligned over long recordings. Desktop audio requires
PulseAudio/PipeWire; under the pure-ALSA fallback only the microphone is
recorded. See `docs/adr/0001-in-process-desktop-mic-mix.md` for the rationale.
