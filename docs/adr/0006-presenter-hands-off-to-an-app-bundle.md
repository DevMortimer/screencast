# `presenter` hands off to an app bundle

## Status

superseded in part by [0008](0008-presenter-is-a-captured-camera-window.md)

Extends [0005](0005-macos-records-the-display-only.md): the presenter is the
system's Presenter Overlay, and this records how the overlay is made reachable
from every launcher.

## Context

Presenter Overlay is offered in Control Center's Video Effects panel, which
lists the apps currently using the camera. macOS decides what "app" means by
the responsible process, an identity that is inherited down the process tree
at spawn time and cannot be changed from inside the process afterwards.

Launched from Terminal, `screencast presenter` inherits Terminal.app's
identity; the panel shows the stream under "Terminal" and the overlay works.
Launched from skhd (a launchd agent) or from a zellij pane (children of the
detached zellij server), there is no app bundle anywhere on the responsibility
chain. The camera opens, its indicator lights, ScreenCaptureKit records — but
Control Center has nothing to list the stream under, so the Video Effects
entry never appears and the overlay is unreachable.

An earlier attempt bootstrapped NSApplication and walked the activation
policy to Accessory, on the theory that Window Server registration was the
missing piece. It was not: AppKit setup changes what the process can do, not
who macOS says it is. The symptom — camera green, no Video Effects entry —
is attribution failing, and attribution is fixed at launch.

## Decision

`make install` builds `Screencast.app`, a minimal bundle around the same
binary: an Info.plist with a bundle identifier, `LSUIElement` so it never
shows a Dock icon, and the camera/microphone usage strings TCC requires of
bundled apps.

When `screencast presenter` finds it would become the recording daemon and
its main bundle has no identifier, it does not record. It launches
`~/Applications/Screencast.app` through LaunchServices (`open --args
presenter`) and exits. launchd starts the bundle as its own responsible
process, so every launcher — Terminal, zellij, skhd — converges on the same
identity, and Control Center has an app to pin the Video Effects entry on.

The handoff is best-effort: with no bundle installed, the process records in
place exactly as before, minus the overlay.

## Consequences

- The overlay is available regardless of what launched the recording, which
  was the point.
- The control socket lives at a fixed path under `$HOME`, so `screencast
  stop` reaches the bundled daemon from any context unchanged.
- LaunchServices attaches no terminal. The bundled daemon appends its output
  to `~/Library/Logs/screencast.log` itself — the same file the skhd bindings
  already redirect to — unless stderr is a tty (running the bundle's binary
  by hand for debugging).
- Privacy permissions move: the first bundled run prompts for Camera,
  Microphone, and Screen Recording under "Screencast" rather than under the
  launcher. The bundle is ad-hoc signed, and an ad-hoc seal names the exact
  build, so TCC asks again after a rebuild. `CODESIGN_ID=<identity>` signs
  with a stable certificate for anyone who finds that tiresome.
- Display-only recording (`screencast` with no argument) is untouched: no
  camera, no overlay, no reason to pay the handoff.
