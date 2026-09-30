# LASO-Computer

LASO-Computer is a Windows endpoint worker for LASO. It runs as a supervised child process and provides a local security boundary for authorized computer-use operations. LASO owns orchestration, durable jobs and sessions, workflow policy, and approval records; this program owns local Windows interaction and rechecks endpoint policy before every action.

The public implementation is native C++20 for Windows 10/11 x64. It has no Go, Python, .NET, Java, or Electron runtime dependency. It opens no inbound listener and does not implement remote enrollment or outbound endpoint transport. Current public LASO `main` was inspected at `ef072429c96eb9b0964e56a561591a37ccb68fbd`; that revision did not contain the worker-process v1 contract, so interoperability against that revision and approval framing are not yet established. The protocol code in this repository is a local implementation pending a verifiable upstream contract.

## What works in this build

The native provider implements bounded screen capture, pointer movement and clicks, literal keyboard text and key input, Unicode clipboard read/write, visible-window enumeration/focus, bounded Windows UI Automation inspection and control invocation, and restricted direct process execution. The endpoint also has a provider registry with capability schemas, health reporting, timeouts, cancellation tokens, bounded results, and audit attribution.

A Playwright MCP adapter is present as a Windows AppContainer child process, with a fixed MCP operation map and no arbitrary code execution capability. C++ now launches and owns Edge with a unique LASO-managed profile and loopback-only CDP endpoint; MCP attaches to that browser rather than launching it. MCP initialize, tool listing, and the `browser_tabs` attach probe pass after a temporary AppContainer-specific loopback exemption and a version-guarded named-pipe namespace postinstall patch. The first real browser operation still fails with `EPERM` during Node `realpath` of its output directory because AppContainer cannot query metadata on the absolute path's ancestors. The patch was validated against a staged bundle copy, but clean `npm ci`/postinstall has not been tested, so browser support remains blocked and fail-closed. No page operation has passed through LASO-Computer.

## Build

Prerequisites: Windows 10/11 x64, Visual Studio 2022 with the Desktop development with C++ workload and Windows 10/11 SDK, and CMake 3.24 or newer. Open a Developer PowerShell or use a Visual Studio CMake generator:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`laso-desktop-tests.exe` is an interactive-session fixture and is intentionally not part of CTest. Run it only in an interactive Windows session; it creates its own disposable window and restores cursor, foreground window, and clipboard when possible:

```powershell
.\build\Debug\laso-desktop-tests.exe
```

## Configuration and use

Run the executable from a terminal:

```powershell
.\build\Debug\laso-computer.exe --init-config
notepad "$env:LOCALAPPDATA\LASO-Computer\config.json"
.\build\Debug\laso-computer.exe --check-config
.\build\Debug\laso-computer.exe --status
```

The configuration lives under the current user's Local AppData by default. New configurations deny every capability. Grant only the exact capabilities needed and use `require_approval` only when the supervising LASO protocol can provide a verified correlated approval exchange. This implementation currently fails closed for `require_approval`; it does not emit unsolicited worker request frames. Do not place credentials in configuration. `--check-config` reports actual decisions, non-denied capabilities, process allowlist count, and plugin paths without printing secrets.

Without a status/configuration option, the executable reads and writes bounded JSON Lines worker-protocol messages on standard input/output. Diagnostics are written to standard error. The current implementation accepts optional durable/session/context fields without using them. Job `Failed`, `Cancelled`, and `TimedOut` states are represented as valid job results; protocol operation errors are separate. Confirm this behavior against LASO only after LASO publishes the matching protocol contract.

## Security boundary

- Endpoint policy defaults to deny; LASO-side authorization never overrides local policy.
- Providers expose named capabilities and cannot dispatch an implicit unrestricted plugin execution operation.
- External providers are untrusted until configured and policy-enabled. The Playwright adapter uses a fixed operation map, a per-run managed browser profile and temp directory, a sanitized environment, bounded stdio, a Windows AppContainer and a kill-on-close Job Object. MCP attaches to the C++-owned Edge, but actual browser operations still fail closed at `realpath`; the temporary loopback exemption is test-only and must be removed after testing.
- `shell.execute` uses direct process creation, not `cmd.exe` or PowerShell. It requires an exact clean absolute executable path allowlist, bounded arguments/results, a temporary working directory, an explicit environment allowlist, and timeout/cancellation.
- Audit events omit capability arguments, clipboard contents, typed text, URLs, process output, and screenshots. Sensitive values are returned only to the authorized caller when the corresponding capability is allowed.
- Input and output frames are centrally bounded. Truncated process output is explicitly identified; clipboard and bounded structured reads fail rather than silently returning partial data.

See [security](docs/security.md), [architecture](docs/architecture.md), [third-party integration audit](docs/third-party-integrations.md), and [known limitations](docs/roadmap.md).

## Architecture and integrations

LASO remains the orchestrator. LASO-Computer is the endpoint-side policy boundary and native Windows capability host. Browser-native structured automation is intended to use Playwright MCP; Windows applications use UI Automation and Win32 first, with `SendInput` only for low-level input. Microsoft UFO/UFO² informed the structured-automation approach; its code is not embedded. Agent-S and UI-TARS remain possible future visual providers, but neither is a dependency. `browser-use` and Open Interpreter are reference projects only.

Third-party plugins do not receive unrestricted machine access. A provider must declare named capability schemas, pass endpoint policy, get bounded arguments/results and cancellation, and produce an attributed audit event. The current extensibility interface is implemented in-process for native providers; the Playwright provider is a separate restricted process. Dynamic loading of arbitrary native DLL plugins is not supported.

## Limitations

- No remote enrollment, endpoint network transport, lease/reconnect protocol, or remote machine targeting. LASO must supervise the process locally.
- No proven end-to-end integration against the current public LASO revision because it does not publish the worker-process contract expected by this worker. Approval request/response framing remains unavailable and therefore deny-by-default.
- Playwright MCP initialize, tools/list, and `browser_tabs` attachment to C++-owned Edge pass only with the current test-host loopback exemption and the guarded named-pipe namespace patch. The first actual browser operation fails at Node `realpath`: AppContainer `lstat` is denied on each absolute-path ancestor, including the volume root. A narrow read-attributes/traverse ACL validation is pending user acceptance of the UAC prompt; no broad volume read is granted. Clean `npm ci`/postinstall is unverified. No page operation is validated. See [Playwright provider investigation](docs/playwright-provider.md).
- UI Automation coverage depends on each application's accessible control tree and provider support. Foreground focus remains subject to Windows policy.
- Interactive desktop tests require a logged-in user session; GitHub-hosted CI runs only noninteractive core tests.
- No Agent-S/UI-TARS visual reasoning, filesystem transfer, installer/service package, local emergency-stop UI, or remote enrollment.

This repository is an implementation under validation, not a claim of production readiness. See [roadmap](docs/roadmap.md) for exact blocked lanes.

## License

LASO-Computer is licensed under Apache-2.0. The vendored JSON header and optional third-party integrations retain their own licenses; see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and [integration audit](docs/third-party-integrations.md).

## Migration status

The CMake build and public CI no longer depend on or build the Go implementation. The original Go source remains in the checkout temporarily because the user-requested removal is conditioned on behavioral parity; full LASO protocol/approval and Playwright acceptance are still blocked as listed above. It is reference-only and is not part of the C++ runtime.
