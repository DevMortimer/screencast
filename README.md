# Screencast

A small X11 screencast recorder written in C. It records the active monitor,
optional webcam video, and optional microphone audio, then renders a final MP4
with NVIDIA NVENC.

## Features

- Records the monitor containing the mouse pointer.
- Supports display-only, webcam-only, and display-plus-webcam modes.
- Captures microphone audio from PulseAudio/PipeWire `default`, with ALSA
  fallback.
- Adds a circular red recording indicator while capture is active.
- Writes a high-quality intermediate MP4 and transcodes it to a final
  `h264_nvenc` MP4.

## Requirements

- Linux with X11
- GCC and `make`
- FFmpeg development libraries:
  - `libavformat`
  - `libavcodec`
  - `libavdevice`
  - `libswscale`
  - `libswresample`
  - `libavutil`
- X11 development libraries:
  - `x11`
  - `xext`
  - `xinerama`
- FFmpeg CLI available as `ffmpeg`
- Desktop notifications via `notify-send`
- NVIDIA GPU/driver stack with `h264_nvenc` support

On Debian/Ubuntu-based systems, the packages are typically:

```sh
sudo apt install build-essential pkg-config ffmpeg \
  libavformat-dev libavcodec-dev libavdevice-dev libswscale-dev \
  libswresample-dev libavutil-dev libx11-dev libxext-dev \
  libxinerama-dev libnotify-bin
```

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

Run the recorder from an X11 session:

```sh
./screencast
```

Hotkeys:

| Hotkey | Action |
| --- | --- |
| `Win+Shift+D` | Record display and microphone |
| `Win+Shift+W` | Record webcam and microphone |
| `Win+Shift+B` | Record display, webcam, and microphone |
| `Win+Esc` | Stop recording and exit |

Changing to a different recording mode shows a 3-second desktop notification.
Pressing the hotkey for the already-active mode does not send another one.

Recordings are written to the home directory:

- Intermediate capture: `~/screencast_YYYYMMDD_HHMMSS_capture.mp4`
- Final output: `~/screencast_YYYYMMDD_HHMMSS.mp4`

The intermediate file is removed after a successful final render unless
`SCREENCAST_KEEP_CAPTURE` is set.

## Configuration

The recorder can be tuned with environment variables:

| Variable | Default | Description |
| --- | --- | --- |
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
SCREENCAST_WEBCAM_DEV=/dev/video2 SCREENCAST_KEEP_CAPTURE=1 ./screencast
```

## Notes

This program uses X11 APIs directly for capture and global hotkeys. I don't
care about Wayland.
