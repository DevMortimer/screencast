# Presenter is a captured camera window

## Status

accepted

Supersedes [0005](0005-macos-records-the-display-only.md) and [0006](0006-presenter-hands-off-to-an-app-bundle.md) where they delegate the presenter to Control Center. The app-bundle handoff remains, but now gives the local AppKit UI and its privacy permissions a stable process identity.

## Decision

`presenter` opens a 720p AVFoundation session and shows it through `AVCaptureVideoPreviewLayer` in a floating AppKit window. ScreenCaptureKit captures that window as part of the display. The window has no title bar, controls, border, or shadow; it is a square with proportional rounded corners, uses aspect-fill to center-crop the 16:9 camera, is resizable, and snaps to the nearest corner of the display's usable area after a drag. Its corner and width persist between recordings.

The presenter window exists only when a recording starts with `screencast presenter`. It cannot be added to a running display-only recording. The encoder loop runs on a worker thread so the main thread can process AppKit events continuously while recording.

This keeps the screen as the only video clock and does not restore the Metal compositor, camera frame ring, or camera/screen timestamp pairing. Camera motion appears as display motion and causes ScreenCaptureKit to deliver a new frame naturally.

## Considered options

- **Control Center Presenter Overlay** gave good segmentation and low application complexity, but its position, size, and activation were not controllable by this program.
- **Restore the Metal compositor** would make placement controllable in the encoded frame, but would not provide a directly draggable window and would restore a second video clock and pairing machinery.
- **Use a captured AppKit preview window** makes placement direct and native while preserving the single display-capture pipeline.

## Consequences

The overlay can move and resize during recording and works on macOS versions that do not provide Apple's Presenter Overlay. It shows the camera background rather than segmenting the presenter. Presenter mode now pays for a 720p camera session, and a moving camera window makes the display stream deliver frames continuously. Plain display recording still opens no camera and creates no window.
