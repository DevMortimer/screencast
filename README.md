# Screencast

A small Wayland screencast recorder written in C. It records an output via the
`wlr-screencopy` protocol, optional webcam video, and optional microphone
audio, then renders a final MP4 with NVIDIA NVENC.

It targets wlroots-based compositors (niri, Sway, Hyprland, river, …) that
implement `wlr-screencopy-unstable-v1`.

## Features

- Records a Wayland output via `wlr-screencopy` (shm buffers).
- Supports display-only, webcam-only, and display-plus-webcam modes.
- Captures microphone audio from PulseAudio/PipeWire `default`, with ALSA
  fallback.
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
- FFmpeg CLI available as `ffmpeg`.
- Desktop notifications via `notify-send`.
- NVIDIA GPU/driver stack with `h264_nvenc` support.

On Debian/Ubuntu-based systems, the packages are typically:

```sh
sudo apt install build-essential pkg-config ffmpeg \
  libavformat-dev libavcodec-dev libavdevice-dev libswscale-dev \
  libswresample-dev libavutil-dev libwayland-dev wayland-protocols \
  libnotify-bin
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

## Usage

Wayland compositors own global keybindings, so — unlike the old X11 version —
`screencast` no longer grabs hotkeys itself. Instead it is a small
daemon/controller:

```sh
screencast display   # start recording the screen + mic (becomes a daemon)
screencast webcam    # switch the running recorder to webcam + mic
screencast both      # switch to screen + webcam overlay + mic
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

## Configuration

The recorder can be tuned with environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `SCREENCAST_OUTPUT` | first output | Wayland output name to capture (e.g. `DP-1`, `HDMI-A-1`). Match `wlr-randr`/`niri msg outputs` names. |
| `SCREENCAST_DRAW_MOUSE` | `1` | Composite the cursor into the recording; set to `0` to hide it. |
| `SCREENCAST_WEBCAM_DEV` | `auto` | Webcam device path, or `auto` to scan `/dev/v4l` and `/dev/video*`. |
| `SCREENCAST_CAM_FORMAT` | `nv12` | Requested webcam input format. |
| `SCREENCAST_CAM_FPS` | `30` | Requested webcam frame rate. |
| `SCREENCAST_CAM_SIZE` | `1920x1080` | Requested webcam capture size. |
| `SCREENCAST_NVENC_CAPTURE_PRESET` | `p3` | NVENC preset for the real-time intermediate capture. |
| `SCREENCAST_NVENC_CAPTURE_QP` | `12` | Constant QP for the intermediate capture. |
| `SCREENCAST_NVENC_FINAL_PRESET` | `p7` | NVENC preset for the final render. |
| `SCREENCAST_NVENC_FINAL_CQ` | `16` | Constant quality value for the final render. |
| `SCREENCAST_NVENC_FINAL_LOOKAHEAD` | `32` | NVENC final render lookahead. |
| `SCREENCAST_NVENC_FINAL_AQ` | `10` | NVENC adaptive quantization strength. |
| `SCREENCAST_KEEP_CAPTURE` | unset | Keep the intermediate capture file when set to any non-empty value. |

Example:

```sh
SCREENCAST_OUTPUT=DP-1 SCREENCAST_KEEP_CAPTURE=1 screencast display
```

## Notes

Screen capture uses `wlr-screencopy-unstable-v1` with `wl_shm` buffers. The
webcam (v4l2) and audio (PulseAudio/PipeWire) paths use FFmpeg's libavdevice.
