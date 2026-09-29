# Architecture

## Responsibility boundary

| LASO backend | LASO-Computer endpoint |
| --- | --- |
| Orchestration, worker selection, jobs and durable sessions | Local Windows runtime and OS APIs |
| Workflow policy and server-side permissions | Independent endpoint capability policy |
| Durable approval records and cancellation coordination | Fail-closed local approval gate and cooperative cancellation |
| Audit coordination and retained history | Redacted local action attribution |

LASO-Computer is an endpoint worker, not LASO core. The currently intended transport is a locally supervised process using standard input/output; there is no listener or remote endpoint enrollment.

## Native runtime

- `src/main.cpp`: command line and process composition.
- `src/config.cpp`: bounded configuration parsing, validation, and serialization.
- `src/protocol.cpp`: JSONL framing, worker job lifecycle, cancellation, and result semantics.
- `src/plugin.cpp`: named capability provider registry and invocation boundary.
- `src/audit.cpp`: redacted local JSONL events.
- `src/windows_platform.cpp`: Win32 capture, input, clipboard, windows, UI Automation, and direct process execution.
- `src/playwright.cpp`: optional out-of-process MCP adapter.
- `include/laso`: stable subsystem interfaces used by the executable and tests.

The process-protocol implementation centrally limits incoming/outgoing frames to 1 MiB. A valid worker job result in `Failed`, `Cancelled`, or `TimedOut` is still a successful protocol `result` operation. This behavior has unit coverage but has not been validated against a matching public LASO contract.

## Provider contract

Providers declare identity/version, named capability descriptions/schemas, invoke structured JSON with a timeout and cancellation token, report health, and shut down through lifecycle hooks. The registry binds provider capabilities to endpoint policy and records provider attribution. Registration is not authorization. Results are checked against configured bounds at the transport boundary. Native provider DLL loading is deliberately not provided.

## Capability selection

Browser jobs should use browser-native structured DOM operations through Playwright. Native Windows applications should use UI Automation and Win32 state before low-level coordinate input. `SendInput` is the last native interaction primitive when structured controls are unavailable. UFO/UFO² were reviewed as architectural references for UIA/Win32/COM composition and were not embedded. Agent-S and UI-TARS are possible future visual fallback providers, not current dependencies.

The Playwright MCP child has explicit Node/server/browser paths, a sanitized environment, a per-run managed profile/temp directory, AppContainer, Job Object, bounded JSONL stdio, and a narrow operation allowlist. `--preserve-symlinks-main` avoids Node's main-module `lstat('C:\\')` probe without a root ACL. The MCP handshake and fixed-tool check succeed, but Edge process creation with Playwright's `--remote-debugging-pipe` stalls during the real browser startup probe. The provider stays unhealthy and browser capabilities are unavailable; browser-native automation is therefore not currently working.

## Protocol status

The current public LASO repository was inspected at `ef072429c96eb9b0964e56a561591a37ccb68fbd`. That revision did not expose a worker-process v1 protocol implementation/documentation. This repository's protocol is consequently pending upstream contract verification. Optional durable/session/context submit fields are accepted. Worker-originated approval framing is intentionally not guessed; endpoint approval-required actions are denied until LASO supports and documents correlated request/response framing.

## Not implemented

Remote enrollment/transport, leased remote endpoints, dynamic DLL loading, filesystem transfer, Agent-S/UI-TARS visual interaction, and a protected local emergency stop surface remain future work.
