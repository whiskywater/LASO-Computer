# LASO-Computer

LASO-Computer is a Windows endpoint worker for LASO. It runs as a supervised child process and provides a local security boundary for authorized computer-use operations. LASO owns orchestration, durable jobs and sessions, workflow policy, and approval records; this program owns local Windows interaction and rechecks endpoint policy before every action.

The public implementation is native C++20 for Windows 10/11 x64. It has no Go, Python, .NET, Java, or Electron runtime dependency. It opens no inbound listener and does not implement remote enrollment or outbound endpoint transport. Its stdin/stdout worker supports the process-worker v1 lifecycle operations: `hello`, `submit`, `status`, `result`, `cancel`, and `shutdown`, using bounded newline-delimited JSON. Submitted capability inputs still pass through endpoint-side policy. `require_approval` uses a correlated Core approval exchange and fails closed unless it receives an exact approval. Providers can opt specific capabilities into bounded correlated permission and question exchanges through the invocation context; those submit exchanges remain open until provider execution completes.

## What works in this build

The native provider implements bounded screen capture, pointer movement and clicks, literal keyboard text and key input, Unicode clipboard read/write, visible-window enumeration/focus, bounded Windows UI Automation inspection and control invocation, and restricted direct process execution. The endpoint also has a provider registry with capability schemas, health reporting, timeouts, cancellation tokens, bounded results, and audit attribution.

A Playwright MCP adapter runs as a Windows AppContainer child process with a fixed typed operation map. C++ launches and owns Edge with a unique LASO-managed profile and loopback-only CDP endpoint. With no loopback exemption, MCP initialization/tool listing succeeds but the first MCP-to-Edge attachment times out connecting to `127.0.0.1`. A diagnostic run outside AppContainer passed the full browser fixture using the same C++-owned Edge and dynamic endpoint, identifying Windows AppContainer loopback isolation as the current blocker. No exemption was recreated and no UAC was requested. The workaround for Node's AppContainer `realpath` failure uses MCP 0.0.83's documented `--allow-unrestricted-file-access`; the adapter still has no file tool or arbitrary MCP passthrough, and AppContainer read/write/process-launch denial was previously tested. See [Playwright provider](docs/playwright-provider.md) for exact evidence. Clean `npm ci` and postinstall provisioning remain unverified.

## Build

Prerequisites: Windows 10/11 x64, Visual Studio 2022 with the Desktop development with C++ workload and Windows 10/11 SDK, and CMake 3.24 or newer. Open a Developer PowerShell or use a Visual Studio CMake generator:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Before a long unattended development task, run `tools\dev-preflight.ps1`. It never elevates. It checks the optional broker and reports whether the Playwright loopback prerequisite is present; it does not remove that state unless explicitly invoked with `-CleanupLegacyLoopback`. It does not replace the separate Playwright acceptance test. Broker install/update/removal are attended administrator actions; see [unattended Windows development](docs/unattended-windows-development.md).

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

The configuration lives under the current user's Local AppData by default. New configurations deny every capability. Grant only the exact capabilities needed. `require_approval` remains fail-closed unless the worker receives a verified correlated approval from Core. Providers that opt into permission or question interactions must proceed only after the corresponding accepted response (`approved` or `answered`). Do not place credentials in configuration. `--check-config` reports actual decisions, non-denied capabilities, process allowlist count, and plugin paths without printing secrets.

Without a status/configuration option, the executable reads and writes bounded JSON Lines worker-protocol messages on standard input/output. Diagnostics are written to standard error. Job `Failed`, `Cancelled`, and `TimedOut` states are represented as valid job results; protocol operation errors are separate. Optional durable-session context is accepted where defined by the process-worker v1 contract. Live acceptance against a running Core is still outstanding.

## Security boundary

- Endpoint policy defaults to deny; LASO-side authorization never overrides local policy.
- Providers expose named capabilities and cannot dispatch an implicit unrestricted plugin execution operation.
- External providers are untrusted until configured and policy-enabled. The Playwright adapter uses a fixed operation map, a per-run managed browser profile and temp directory, a sanitized environment, bounded stdio, a Windows AppContainer, and a one-process kill-on-close Job Object. The MCP process has no network capabilities. Previous AppContainer denial tests rejected generated outside-file read/write and Node's child-process launch. The documented MCP unrestricted-file option disables MCP's workspace/file-URL guard, so OS AppContainer ACLs, C++ HTTP(S) URL validation, and the fixed operation map remain essential security controls.
- `shell.execute` uses direct process creation, not `cmd.exe` or PowerShell. It requires an exact clean absolute executable path allowlist, bounded arguments/results, a temporary working directory, an explicit environment allowlist, and timeout/cancellation.
- Audit events omit capability arguments, clipboard contents, typed text, URLs, process output, and screenshots. Sensitive values are returned only to the authorized caller when the corresponding capability is allowed.
- Input and output frames are centrally bounded. Truncated process output is explicitly identified; clipboard and bounded structured reads fail rather than silently returning partial data.
- An optional typed privileged broker supports status, the fixed Playwright AppContainer SID, and inspection/removal of only a legacy loopback exemption. It has no shell, arbitrary process, ACL-editing, service-control, or exemption-add operation. Install/update/uninstall require an attended administrator session; ordinary preflight and tests never request UAC. See [unattended Windows development](docs/unattended-windows-development.md).

See [security](docs/security.md), [architecture](docs/architecture.md), [third-party integration audit](docs/third-party-integrations.md), and [known limitations](docs/roadmap.md).

## Architecture and integrations

LASO remains the orchestrator. LASO-Computer is the endpoint-side policy boundary and native Windows capability host. Browser-native structured automation is intended to use Playwright MCP; Windows applications use UI Automation and Win32 first, with `SendInput` only for low-level input. Microsoft UFO/UFO² informed the structured-automation approach; its code is not embedded. Agent-S and UI-TARS remain possible future visual providers, but neither is a dependency. `browser-use` and Open Interpreter are reference projects only.

Third-party plugins do not receive unrestricted machine access. A provider must declare named capability schemas, pass endpoint policy, get bounded arguments/results and cancellation, and produce an attributed audit event. The current extensibility interface is implemented in-process for native providers; the Playwright provider is a separate restricted process. Dynamic loading of arbitrary native DLL plugins is not supported.

## Limitations

- No remote enrollment, endpoint network transport, lease/reconnect protocol, or remote machine targeting. LASO must supervise the process locally.
- End-to-end acceptance against a running current Core and Windows worker has not yet been completed. Unit and Windows CI cover the worker protocol, provider dispatch, and correlated interaction framing. Browser containment and clean package provisioning remain unverified; see [Playwright provider](docs/playwright-provider.md).
- Playwright MCP 0.0.83's own `fs.realpath` guard fails under Windows AppContainer because Node/libuv's DOS-volume `GetFinalPathNameByHandleW` step is denied. Node 22 and 24 reproduced it; minimal traverse/read-attributes ACEs did not fix it. No broad `C:\` permission was granted. MCP's documented unrestricted-file option avoids that canonicalization path. Separately, without a loopback exemption the AppContainer MCP's CDP request times out; the identical C++-owned Edge/dynamic endpoint and browser fixture pass in a diagnostic run outside AppContainer. No exemption was recreated. Same-user CDP hijacking remains possible; clean `npm ci`/postinstall and hosted browser CI are not validated. See [Playwright provider](docs/playwright-provider.md).
- A medium-integrity `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)` diagnostic attached to Edge and passed the local fixture, but `IsTokenRestricted` was false and the provider could read and write a generated file outside its runtime. A `WinRestrictedCodeSid` variant did not initialize Node. Restricted-token mode was rejected; AppContainer remains production. See [the experiment and file/process access results](docs/restricted-token-experiment.md).
- UI Automation coverage depends on each application's accessible control tree and provider support. Foreground focus remains subject to Windows policy.
- Interactive desktop tests require a logged-in user session; GitHub-hosted CI runs only noninteractive core tests.
- No Agent-S/UI-TARS visual reasoning, filesystem transfer, broker service install/lifecycle validation without an attended bootstrap, local emergency-stop UI, or remote enrollment.

This repository is an implementation under validation, not a claim of production readiness. The broker service builds and its typed protocol can be tested without elevation; service installation/lifecycle validation requires an attended bootstrap and has not been run by unattended CI. Browser acceptance without the earlier loopback-exemption state is currently blocked. See [roadmap](docs/roadmap.md) for exact blocked lanes.

## License

LASO-Computer is licensed under Apache-2.0. The vendored JSON header and optional third-party integrations retain their own licenses; see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and [integration audit](docs/third-party-integrations.md).

## Migration status

The CMake build and public CI build and test the native C++ worker. The executable contains the C++ runtime and its declared native dependencies.

## Acceptance-only worker

Windows CI also builds `laso-computer-acceptance.exe` when `LASO_BUILD_ACCEPTANCE_WORKER=ON`. This separate executable adds inert capabilities for exercising correlated permission, question, and cooperative cancellation flows against Core. The option defaults off; the acceptance executable is not part of the normal worker or packaging and must not be deployed.
