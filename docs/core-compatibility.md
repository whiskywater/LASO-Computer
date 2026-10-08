# LASO Core process-worker compatibility

This note records the private LASO Computer implementation against the
authoritative `Registered-Agent-Attorney/LASO` process-worker protocol. The
initial compatibility audit used `cbdd8e2d5ca1f868c3364f98329291f39c1c85cf`;
final consolidation rechecked Core `main` at
`be325488aa712dd253773681fd66302785c7f182` on 2026-09-29. It describes the
local supervised process-worker boundary, not a remote endpoint connection.

## Current v1 contract

Core and Computer use protocol version 1 with newline-delimited JSON and the
`hello`, `submit`, `status`, `result`, `cancel`, and `shutdown` operations.
Computer returns Core `WorkerMetadata` in `hello.metadata`, while retaining its
endpoint identity and per-capability availability/policy details in the
`computer` extension and its earlier capability listing in `hello.payload`.

`submit` accepts both the original Computer payload (`capability`, `arguments`,
and optional `principal`) and Core's current `WorkerRequest` payload. Core's
`input` becomes the structured arguments passed to the named local capability.
The LASO job ID is the authoritative job identity; a conflicting payload job
ID is rejected. A nonempty `durable_session_id` is copied into the redacted
local audit identity. Continuation and session-context values are accepted
within protocol bounds but are not used to control the desktop or published as
provider continuation.

Every action still passes through the endpoint's local per-capability policy.
Discovery of a capability is not authorization. `deny` remains the default;
`require_approval` sends a correlated `permission` request and proceeds only
after an explicit approved response. Computer reports worker-declared
`Failed`, `Cancelled`, and `TimedOut` states with protocol `ok: true`; malformed
or unsupported protocol messages use `ok: false`. Results and frames are
bounded, and values returned after cancellation or an explicit `timeout_ms`
expiry are discarded.

The worker handshake advertises `supports_status: true` for polling jobs during
the current connection and `supports_recovery: false` because the endpoint's
job table is in memory and cannot be reconciled after a process restart.

Core's current worker capability advertisement is used only for worker
selection. Computer advertises locally available capability names and includes
each capability's local decision in its `computer.capability_status` extension.
The local decision is enforced again for every invocation.

## Compatibility boundaries

- **Compatible:** v1 framing and operations, Core hello metadata, Core submit
  fields, worker result states, durable job/session identifiers, local
  permission requests, bounded result transport, and cancellation.
- **Not implemented by Computer:** remote enrollment or outbound endpoint
  transport, reconnect leases/heartbeats, durable action recovery across
  endpoint process restart, browser DOM control, filesystem transfer, and
  provider continuation/session-context execution. Computer does not advertise
  Core's `session-context` capability, so Core must not route durable provider
  context turns to it. The action ledger is process-local: after restart, an old
  external job ID reports `Unknown` without returning a result or repeating the
  action. Core now persists this ambiguous outcome as `Unknown`, including
  after manager restart when submission began but no external job ID was
  recorded. Unknown jobs are not replayed or resubmitted. Core's regression
  coverage exercises lost child-process jobs and restart during ambiguous
  submission.
- **Deadline behavior:** Core places a submission timestamp in `deadline` and
  sends the remaining run budget as `timeout_ms`, capped by the configured
  process-worker request timeout. Computer treats `deadline` as metadata and
  applies the bounded `timeout_ms` to the action context. It discards a late
  handler result after cancellation or timeout, but an uncooperative OS call
  cannot be forcibly interrupted by Go context cancellation.
- **Version discovery:** Core's `/api/v1/version` and advertised capability
  array describe server API behavior. Process-worker protocol version 1 is a
  separate contract; Computer does not make an HTTP version request because it
  runs as a locally supervised child process.

## Computer capability inventory

The private Go implementation currently registers `screen.capture`,
`pointer.move`, `pointer.click`, `keyboard.type`, `keyboard.key`,
`clipboard.read`, `clipboard.write`, `window.list`, `window.focus`,
`browser.navigate`, and `shell.execute`. Availability is platform/session
dependent and separately reported. Shell execution has its own exact-path
allowlist and is denied unless configured. This inventory does not imply that
any action is locally authorized.
