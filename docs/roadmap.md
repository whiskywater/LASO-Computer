# Roadmap

## Implemented in this foundation

- Go runtime and build/test targets for Windows amd64 and Linux amd64.
- Persistent random client identity, capability discovery, structured configuration, and deny-by-default local policy.
- Guarded capability dispatch, approval handoff, request replay protection, cancellation, and redacted audit.
- Windows screen capture, pointer move/click, keyboard text/key input, text clipboard, visible-window list/focus, and default-browser URL opening.
- Linux X11 screen capture, pointer movement, window list/focus, and default-browser URL opening. The XTEST click and keyboard adapters compile and are detected, but real input events were not observed in the authorized Linux smoke test; this portion remains incomplete. Plain-text clipboard is available when `xclip` is installed.
- Separately gated shell execution by exact executable path, with argument arrays, timeout/cancellation, empty-by-default environment, and bounded stdout/stderr.
- LASO local process-worker v1 framing and cross-platform CI.

## Next MVP work

1. Add the outbound authenticated endpoint channel in LASO and a matching client transport, including enrollment, machine targeting, leases, heartbeat, reconnect, cancellation, replay protection, and grant revocation.
2. Add a small local user control surface for connected state, current agent/session, granted capabilities, pause, revoke, disconnect, and cancel. Keep Admin View as a separate policy and audit surface.
3. Add policy versioning and live revocation so permission changes stop queued work and cancel incompatible in-flight actions.
4. Fix and verify Linux XTEST click and keyboard event delivery across supported X11 distributions, then design a safe Wayland portal/native path; add Linux clipboard support without requiring optional external commands where practical.
5. Add app/window bounds, safer application launch/close rules, and visible action feedback.
6. Add filesystem scopes, approved directories, upload/download staging, and artifact references through LASO's existing artifact/job boundaries.
7. Add browser-native automation as a separate module with redirect/state tracking and strict same-user authentication behavior; keep visual control as a fallback.
8. Add installer/service packaging, signed releases, migration/uninstall behavior, hardened service identity, and operational diagnostics.

## Later advanced capabilities

- Structured desktop observations and task/action traces.
- Multi-monitor display selection and image tiling.
- File chooser workflows and bounded upload/download actions.
- More complete native UI automation and window ownership metadata.
- Enterprise policy composition from organization through machine and capability.
- Independent local emergency stop control that remote agents cannot disable.
