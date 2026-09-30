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

On the Windows test host, the pinned MCP package's guarded named-pipe namespace patch is present. The first browser call also exposed a Node/libuv `GetFinalPathNameByHandleW(VOLUME_NAME_DOS)` AppContainer limitation; ACL experiments did not fix it and no broad volume permission was granted. MCP's documented `--allow-unrestricted-file-access` option skips its own canonical-path guard; AppContainer ACLs, the one-process Job Object, typed C++ tools, and C++ URL validation remain in force. Current Debug testing found AppContainer MCP's HTTP request to the valid local CDP endpoint times out when the loopback-exemption list is empty. The same C++-owned Edge, port-0 discovery, MCP package, and complete fixture pass outside AppContainer for diagnosis, so the endpoint form and Edge process limits are not the cause. The production sandbox remains enabled; no exemption was restored. Clean npm install and hosted browser acceptance remain unverified. See [the provider record](playwright-provider.md).

An optional LocalSystem development broker is a separate executable and service with a fixed named-pipe RPC surface. It supports only status, fixed AppContainer SID lookup, and inspection/removal of that AppContainer's legacy loopback exemption. It is installed only during attended bootstrap, authorizes the bootstrapped interactive user SID, rejects remote pipe clients, audits operations, and offers no generic elevation facility. See [unattended Windows development](unattended-windows-development.md).

## Protocol status

The current public LASO repository was inspected at `ef072429c96eb9b0964e56a561591a37ccb68fbd`. That revision did not expose a worker-process v1 protocol implementation/documentation. This repository's protocol is consequently pending upstream contract verification. Optional durable/session/context submit fields are accepted. Worker-originated approval framing is intentionally not guessed; endpoint approval-required actions are denied until LASO supports and documents correlated request/response framing.

## Not implemented

Remote enrollment/transport, leased remote endpoints, dynamic DLL loading, filesystem transfer, Agent-S/UI-TARS visual interaction, and a protected local emergency stop surface remain future work.
