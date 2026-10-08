# Architecture

## Responsibility boundary

| LASO backend | LASO Computer endpoint |
| --- | --- |
| Agent/task orchestration and worker selection | OS desktop APIs and local process execution |
| Durable job and session ownership | Screen, pointer, keyboard, clipboard, and window operations |
| Organization, user, agent, job, and worker policy | Local capability registry and a second authorization check |
| Approval requests and durable decisions | Pause an action while LASO resolves an approval |
| Worker leases, heartbeats, cancellation, and recovery | Cooperative cancellation and sanitized results |
| Audit/control-plane event persistence | Redacted endpoint execution audit |

Desktop-control implementations stay in this repository. The LASO backend should dispatch an authorized worker request and retain durable ownership; it should not call OS desktop APIs itself.

## Current integration boundary

LASO documents a versioned newline-delimited JSON process-worker protocol (v1). LASO supervises this executable, sends one request frame at a time, and owns the durable worker-job identifier. LASO Computer implements the protocol's `hello`, `submit`, `status`, `result`, `cancel`, and `shutdown` operations. A `submit` payload contains a capability name, structured arguments, and optional agent/session identifiers. Each capability request goes through the local policy-enforcing executor.

The process protocol already carries durable job/session context and a narrow worker-originated `permission` request/response. The endpoint uses that for `require_approval`; it never auto-approves. Frames, jobs, output, and approval waits are bounded. A repeated request ID with identical bytes replays the initial response. Reusing an ID with different content is rejected. LASO's worker manager remains responsible for durable deduplication across process restarts; an ambiguous operation after restart must remain unknown rather than being resubmitted automatically.

The concrete protocol is documented in LASO's `docs/worker-process-protocol.md` and worker lifecycle in `docs/workers.md`. Those repositories remain unchanged by this project.

## Runtime modules

- `cmd/laso-computer`: command-line status/configuration commands and process lifetime.
- `internal/catalog`: capability names and descriptions.
- `internal/capabilities`: registration, guarded dispatch, argument validation, OS-neutral handlers, and shell restrictions.
- `internal/policy`: local `allow`, `require_approval`, and `deny` decisions.
- `internal/transport`: bounded stdio framing and correlated LASO permission requests.
- `internal/client`: process-worker job state, cancellation, result handling, and request replay protection.
- `internal/platform`: separate Windows API and Linux X11 drivers.
- `internal/audit`, `internal/config`, and `internal/identity`: local audit, configuration validation, and random persistent client identity.

The executor receives platform handlers only through the registry. Transport callers do not receive raw platform-driver methods, so invoking a capability still passes through local authorization.

## Current remote endpoint path

The Windows deployment runs the existing versioned LASO worker protocol over a loopback TCP listener. The `--supervise` mode keeps an authenticated SSH reverse tunnel connected to the Server1300 loopback port, where Core's process worker bridges the protocol to the endpoint. Both the worker listener and health endpoints bind only to IPv4 loopback; they are not exposed on the LAN or the public network. SSH supplies transport encryption and authenticates the connecting workstation. Restrict the SSH identity to the one remote-forward port and follow the deployment steps in [`remote-loopback-worker.md`](remote-loopback-worker.md).

The executor and local capability policy remain active on the endpoint for every request. This transport does not add application-level TLS, endpoint enrollment, or capability-grant versioning and revocation; it relies on a controlled SSH identity and loopback-only forwarding.

```text
LASO orchestration and policy
  → loopback worker listener behind an authenticated SSH reverse tunnel
  → endpoint request validation and local policy
  → registered capability handler
  → Windows or Linux API
```

The worker protocol retains bounded frames, replay protection, and cancellation. The tunnel supervisor reconnects after transport loss; an in-flight action whose response is lost remains uncertain and must be reconciled rather than replayed. Endpoint enrollment and capability-grant versioning/revocation are not part of this SSH-based deployment.
