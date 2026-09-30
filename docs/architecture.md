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

Playwright uses split containment: C++ directly launches the configured Edge executable into a unique LASO-managed profile and owns its process tree in a kill-on-close Job Object; a separate AppContainerized MCP process attaches to that browser through CDP. The C++ runtime selects an ephemeral port, asks Edge to bind only `127.0.0.1`, verifies `/json/version` and the owning listener PID, then configures pinned MCP 0.0.83 through its shipped `browser.cdpEndpoint` option. MCP has no network capabilities and its job permits only the provider process, preventing it from spawning Edge.

On the Windows test host, a temporary AppContainer-SID loopback exemption and a guarded pinned-package named-pipe namespace patch allowed initialize, tools/list, and `browser_tabs` attachment. The first real browser operation still fails with `EPERM` because Node cannot read attributes while resolving the AppContainer output path's ancestors. A narrowly scoped metadata/traverse ACL test is pending manual UAC acceptance; no broad volume read or data access is granted. The patch script is wired to npm `postinstall` and passed staged-copy/idempotence checks, but clean install has not been tested. Browser capabilities must remain fail-closed until path access and actual operations pass. See [the provider investigation](playwright-provider.md).

## Protocol status

The current public LASO repository was inspected at `ef072429c96eb9b0964e56a561591a37ccb68fbd`. That revision did not expose a worker-process v1 protocol implementation/documentation. This repository's protocol is consequently pending upstream contract verification. Optional durable/session/context submit fields are accepted. Worker-originated approval framing is intentionally not guessed; endpoint approval-required actions are denied until LASO supports and documents correlated request/response framing.

## Not implemented

Remote enrollment/transport, leased remote endpoints, dynamic DLL loading, filesystem transfer, Agent-S/UI-TARS visual interaction, and a protected local emergency stop surface remain future work.
