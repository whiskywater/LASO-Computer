# LASO Computer

LASO Computer is the endpoint-side computer-use worker for the LASO ecosystem. It keeps desktop and local-process control on the machine being controlled. LASO remains responsible for orchestration, durable jobs and sessions, worker selection, server-side policy, approvals, and cancellation coordination.

This repository is an early development client, not a production remote-access agent. It is a Go executable for Windows and Linux. Its default LASO integration boundary is the versioned local process-worker protocol over standard input and output. An optional loopback-only listener can serve that same protocol behind a restricted authenticated SSH reverse tunnel; it never binds to a LAN or public interface.

## Current foundation

The client provides:

- A persistent random client ID stored in the current user's application-data directory. It is not derived from hardware identifiers.
- A discoverable capability registry and a local `allow` / `require_approval` / `deny` policy. The default is `deny`.
- A guarded executor that checks local policy before every capability handler.
- Redacted JSON-lines audit events with the requester IDs, capability, decision, outcome, and timing. Arguments, typed text, clipboard values, shell arguments, and URLs are not recorded.
- LASO worker-process protocol v1 framing, bounded to 1 MiB per input frame, with duplicate request-ID protection and cooperative job cancellation.
- A managed loopback listener and SSH reverse-tunnel supervisor with keepalives, bounded exponential reconnect, degraded health reporting, and private service logs. SSH credentials stay in the operating-system SSH agent/configuration.
- Windows screen capture, pointer, keyboard, plain-text clipboard, visible-window discovery/focus, and default-browser navigation.
- Linux X11 screen capture, pointer movement, window discovery/focus, and default-browser navigation. The XTEST click and keyboard adapters are present, but the authorized Linux smoke test did not observe button or key events in a focused test window; treat Linux interaction as incomplete until that is resolved. Linux plain-text clipboard requires `xclip` in the user session.
- The Linux X11 keyboard adapter maps ASCII through the active keyboard layout; event delivery remains unverified in the tested session, as noted below.
- A separately grantable shell capability with exact executable allowlisting, direct argument-vector execution, an empty child environment except configured variable names, bounded output, timeout, and cancellation.

The full capability list and per-machine availability are reported by `--status` and `--capabilities`. Platform support can depend on the active graphical session. Linux Wayland-native input is not implemented.

## Build

Use Go 1.25 or newer.

```sh
go test ./...
go vet ./...
go build ./cmd/laso-computer
```

Cross-build the two development targets with:

```sh
GOOS=windows GOARCH=amd64 go build -o laso-computer.exe ./cmd/laso-computer
GOOS=linux GOARCH=amd64 go build -o laso-computer ./cmd/laso-computer
```

CI runs tests and builds on Windows amd64 and Linux amd64.

## Development run

```sh
go run ./cmd/laso-computer --status
go run ./cmd/laso-computer --capabilities
go run ./cmd/laso-computer --init-config
go run ./cmd/laso-computer --check-config
```

The sample configuration denies every capability. Edit the local configuration to grant only the capabilities needed for a specific test or deployment. `--init-config` refuses to overwrite an existing file. With no status/configuration command, the process reads and writes the LASO worker protocol on stdin/stdout; diagnostics go to stderr. For remote use, run the managed path from a signed-in endpoint user's logon task:

```powershell
laso-computer.exe --config "$env:APPDATA\LASO\Computer\config.json" `
  --supervise --ssh-host server1300
```

The SSH alias must use the approved host, strict host-key verification, and a restricted reverse-forward identity. The default worker listener, health listener, and remote port bind to loopback only. See [remote loopback worker](docs/remote-loopback-worker.md) for SSH configuration and Task Scheduler settings.

## Relationship to LASO

LASO's worker manager and supervised process transport own job dispatch, durable sessions, server policy, approval records, leases, and recovery. LASO Computer owns OS APIs and local enforcement. The client uses the LASO process-worker v1 operations (`hello`, `submit`, `status`, `result`, `cancel`, and `shutdown`). When local policy requires approval, it sends a correlated `permission` worker request through the protocol and waits for LASO's decision. A missing or negative decision denies the action.

See [architecture](docs/architecture.md), [security model](docs/security.md), [browser confinement](docs/browser-confinement.md), and [roadmap](docs/roadmap.md). Remote loopback forwarding remains a deployment-owned integration path; SSH credentials, user enrollment, and server forwarding policy remain outside the client.

## Limitations

- There is no remote enrollment, LASO-managed machine targeting, or endpoint lease/revocation protocol yet. The reconnect mechanism supervises the configured authenticated SSH reverse tunnel and relies on the operating-system SSH identity.
- The endpoint has no GUI, tray control, pause/revoke button, or live policy reload. A process signal and LASO cancellation are the available stop paths in this foundation.
- Windows may refuse to focus a background window under its foreground activation policy. A local click can activate the selected window; the client does not bypass that OS policy.
- Linux screen/window support is X11 based. The test session accepted pointer movement, but its focused test app did not observe click or keyboard events even though the XTEST requests were accepted. Do not rely on Linux click or keyboard actions until input delivery is fixed and retested. Wayland-native control is deferred. Clipboard access requires `xclip`.
- Browser support opens a validated HTTP(S) URL with the operating system's default browser. It does not yet automate DOM state, observe browser-native navigation, or manage uploads/downloads.
- Filesystem read/write, file transfer, application launch/close policy, and general native UI automation are not implemented.
- Local identity and configuration are protected by the current user's filesystem permissions, not by a separate service account or hardware security module.

Do not grant `shell.execute` broadly. Keep the supervised endpoint default-deny for capabilities that are not explicitly approved. The supervisor still requires integrated LASO Core acceptance before a release can be called production-ready.
