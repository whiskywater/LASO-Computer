# Security model

## Local policy

Every registered capability invocation enters the guarded executor. The executor looks up the registered name, reads the local decision, handles any approval request, checks platform availability, then invokes the handler. Unknown capabilities and invalid parameters fail closed. The initial configuration defaults to `deny` for every capability.

Each capability can be set to `allow`, `require_approval`, or `deny`. The endpoint does not treat a LASO-side grant as sufficient. A local `require_approval` decision sends a narrow `permission` request over the supervised LASO worker protocol and proceeds only after an explicit approved response. If the approval channel is absent, closed, denied, or times out, the invocation is denied. Local configuration is loaded at process start; changing it does not revoke a running invocation until the process is restarted or the action is cancelled.

The sample configuration grants nothing. `shell.execute` also requires an exact absolute executable path in a separate allowlist. Shell commands are never assembled into a shell string. The child receives no inherited environment by default; only explicitly named environment variables are copied. Shell jobs start in the operating system's temporary directory instead of inheriting the client's working directory. Arguments and output have size limits, and execution has a bounded timeout and context cancellation.

## Input validation and limits

- Stdio frames and structured payloads are bounded to 1 MiB. Unknown top-level protocol fields, unsupported protocol versions, malformed JSON, duplicate JSON object keys, invalid IDs, and duplicate IDs with changed content are rejected. The worker fails all pending interactions closed if a malformed response has ambiguous correlation.
- Pointer coordinates are nonnegative primary-display pixels and are checked against the display bounds by the platform driver. Click count is limited to one or two.
- Windows foreground activation follows the operating system's focus policy. The endpoint does not use focus-lock workarounds; a local user click may be needed before a background window can be activated.
- Browser navigation accepts only HTTP(S) URLs without embedded credentials. It uses the operating system's default browser and does not bypass browser authentication or security controls.
- Shell paths must be clean absolute paths and match the configured allowlist exactly. Execution uses an argument vector, not shell interpolation.
- The process worker retains at most 64 active/recent jobs, keeps a 4,096-request replay ledger and job-ID tombstones, and bounds output and approval waits. It fails closed when a ledger limit is reached. LASO remains the durable source of job IDs and cross-process recovery state; the supervisor should reconcile and replace a saturated process before new work.

## Audit and sensitive data

The append-only local JSONL audit records request, agent, session, and job IDs; capability; policy decision; outcome; duration; and an explicit redaction marker. It never records capability arguments, typed text, clipboard contents, screenshots, shell arguments, stdout/stderr, or full URLs. Audit files and local configuration are created with current-user-only permissions where the operating system supports them. Audit write errors do not change the capability's authorization decision; deployments should monitor local storage health.

Screen images are encoded in memory and returned inline only after `screen.capture` is authorized. The client does not persist captures. Images are reduced in size if needed to fit a bounded response. Window titles, clipboard contents, and command output are returned only to the authorized caller, not copied to the audit log.

## Transport and trust

LASO can supervise the endpoint as a local child process over stdin/stdout. The Windows deployment also supports the versioned worker protocol on an IPv4 loopback TCP listener behind a supervised, authenticated SSH reverse tunnel. The listener and health endpoints do not bind to a LAN or public interface. SSH provides transport encryption and workstation authentication; configure a dedicated SSH identity restricted to the one remote-forward port. See [`remote-loopback-worker.md`](remote-loopback-worker.md) for the supported setup.

The endpoint applies its local capability policy to every request regardless of transport. The SSH-based deployment does not provide application-level TLS, endpoint enrollment, or capability-grant versioning and revocation. Do not expose the listener beyond loopback or use an unrestricted SSH identity. No GitHub token, LASO service token, or browser profile is used by the endpoint process.

## Human stop control

LASO cancellation reaches the running job, and the local process handles an interrupt/termination signal. The endpoint has no independent always-visible pause/revoke control or protected emergency hotkey yet. A remote controller must not be given a way to disable the eventual local stop control; that requires a separate local UI/input path in a later milestone.
