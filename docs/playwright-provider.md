# Playwright provider

## Windows implementation and current status

LASO-Computer C++ launches and owns an isolated Edge process. It then starts the pinned Playwright MCP provider in a Windows AppContainer and connects that provider to the Edge instance over CDP on loopback. MCP does not launch or select a browser. Browser capabilities are exposed only through the C++ endpoint policy and a fixed typed adapter.

```mermaid
flowchart LR
  L[LASO] --> C[LASO-Computer C++]
  C -->|launches, owns, and stops| E[Managed Edge with unique profile]
  C -->|launches in AppContainer| M[Playwright MCP 0.0.83]
  M -->|CDP over loopback| E
  C -->|fixed typed capabilities and endpoint policy| L
```

Tested locally on Windows 11 x64 with MSVC/Visual Studio 2022, Node.js 24.21.0 x64, `@playwright/mcp` 0.0.83, its bundled Playwright `1.64.0-alpha-1790635538000`, and Microsoft Edge 154.0.4258.37 / Chromium 154.0.8037.58. Node 22.23.3 reproduced the same original AppContainer `realpath` failure; the final browser fixture has so far been run with Node 24.21.0.

## Root cause and chosen workaround

The first real MCP browser operation generated a snapshot under the provider's AppContainer-owned output directory:

```text
%LOCALAPPDATA%\Packages\<app-container>\AC\ms-playwright\LASO-Computer\plugin-temp\<run-id>\...
```

Node reported `EPERM` from `fs.realpath`. A local native Win32 probe showed that opening the path with `CreateFileW` succeeds, but `GetFinalPathNameByHandleW(..., VOLUME_NAME_DOS)` returns `ERROR_ACCESS_DENIED` (5). The same handle succeeds with the NT or volume-less name modes. This is Windows AppContainer volume-name resolution behavior, not denial of file data access. Node 22 and 24 behaved the same way. Explicit non-inheriting `FILE_TRAVERSE`, `FILE_READ_ATTRIBUTES`, and combined ACE experiments on exact ancestors did not fix it; the ACLs were restored. No permission was added to `C:\` or its descendants.

The implementation uses the documented `@playwright/mcp` 0.0.83 `--allow-unrestricted-file-access` option so MCP skips its own workspace-root and `file://` guard that depends on the failing canonicalization. That option weakens MCP's own guardrail. The endpoint therefore relies on the OS AppContainer ACLs and Job Object for provider containment, plus the C++ capability boundary: C++ accepts only HTTP(S) in `browser.navigate`, exposes no MCP filesystem or arbitrary-tool passthrough, and maps only fixed browser tools. Do not remove the AppContainer, its narrow ACLs, or the one-process Job Object. Do not compensate with a broad filesystem ACE.

This is a documented upstream workaround, not a fix to Node's realpath behavior. Microsoft's [STL issue #6286](https://github.com/microsoft/STL/issues/6286) describes the same AppContainer failure in `GetFinalPathNameByHandleW` DOS-volume translation; a direct local probe confirmed the corresponding Win32 behavior here. `libuv`'s Windows `realpath` path uses the DOS-volume mode ([source](https://github.com/libuv/libuv/blob/v1.x/src/win/fs.c)).

Containment checks were performed from the actual provider process with generated test material outside the provider runtime:

- Reading a generated `C:\Users\Public` test file: denied (`EPERM`).
- Writing a sibling generated file there: denied (`EPERM`).
- Spawning `whoami.exe` from Node: denied (`UNKNOWN` from Node's `spawn`), consistent with the provider Job Object's one-active-process limit.
- Direct `browser.navigate` to `file://`: rejected by the C++ URL validator.
- Clicking an HTTP fixture link to the generated `file://` test file: Edge 154 remained on the HTTP fixture; subsequent snapshot contained no test marker.

These checks do not claim protection from another process running as the same Windows user, an administrator, or a compromised Edge/browser sandbox. The flag disables an MCP guardrail, so changes to the tool list or capability mapping require renewed security review.

## Process ownership and profile

The C++ runtime creates a unique `%LOCALAPPDATA%\LASO-Computer\browser-runs\<run-id>` directory and `--user-data-dir`, chooses an ephemeral loopback port, and launches the administrator-configured Edge executable directly with fixed arguments. It never selects the user's normal Edge profile, cookies, password store, extensions, or history. Edge's own Chromium sandbox remains enabled; `--no-sandbox` is not used. A kill-on-close Job Object owns Edge's process tree and limits process count and memory. The executable must be an absolute existing path with non-reparse components.

The runtime checks `/json/version`, the browser identity, `DevToolsActivePort`, loopback binding, and that the listener PID matches the launched Edge process. CDP uses HTTP/WebSocket on `127.0.0.1` with a dynamically selected port. This Edge configuration has no CDP authentication. Another process under the same user could discover and connect to the short-lived port; the endpoint is not logged or returned to LASO. This residual local-user risk is documented, not eliminated by the random port.

The provider receives only the necessary environment, explicit stdio handles, an AppContainer SID, and no AppContainer network capabilities. Its Job Object allows one process, so MCP cannot launch Edge or arbitrary helper processes. Edge itself performs website DNS/TLS/network traffic. An AppContainer-SID-specific Windows loopback exemption is currently required for MCP to connect to Edge's CDP listener. That exemption allows this AppContainer loopback access generally rather than constraining it to the ephemeral CDP port. Removal is pending one manual elevated cleanup; do not leave it as an undocumented permanent setting.

## Capability boundary

The C++ adapter invokes explicit MCP tool names only: navigate, snapshot, text query, click by snapshot reference, fill, select, tabs, back, and screenshot. The endpoint policy is checked before each action. MCP's own tools are never passed through by name. The endpoint does not expose arbitrary JavaScript/Playwright execution, shell, file access, cookies/storage, upload, or download capabilities. A provider being installed or healthy does not allow a capability that endpoint policy denies.

## Local validation

The actual C++ → AppContainer MCP → CDP → C++-owned Edge path passed a deterministic local fixture in both clean Debug and Release builds for provider startup, navigation, structured snapshot, fill, click, resulting DOM state, select, tab listing, screenshot, and link navigation. A separate public HTTPS check navigated to `https://example.com/` and verified the structured snapshot. Direct `file://` navigation was rejected by C++; in a separate test, clicking an HTTP fixture link to a generated public test file did not navigate Edge to the file or expose its marker. With a generated outside file, Node's read and write attempts were denied (`EPERM`), and its attempt to spawn `whoami.exe` failed under the one-process Job Object.

Clean Debug and Release configure/build/CTest passed. The disposable Windows desktop fixture passed in both configurations; keyboard/mouse injection and clipboard read/write were skipped because the fixture did not receive foreground focus and the clipboard was unavailable/non-text. A hung navigation returned a failed job after the 20-second MCP timeout and a following `browser.tabs` call succeeded. Cancellation returned a valid `Cancelled` result and tore down the provider/browser. Killing Edge or MCP made browser capabilities unavailable without corrupting worker protocol; restarting LASO-Computer restored them. ACL checks showed no leftover AppContainer ACE on the Node/package directories and no per-run provider output directory remained. Hosted CI is not yet rechecked. The temporary loopback exemption must be removed after the final browser run; issue #2 remains open until cleanup and final hosted validation are complete.

## Reproduction and cleanup notes

The package version is checked at startup and pinned to 0.0.83; production configuration must not use `@latest`. Provider dependencies are provisioned beneath the administrator-managed LASO-Computer runtime. Browser profiles and per-run MCP output are unique and removed during normal shutdown. Clean-install provisioning is documented in the repository; browser acceptance requires an interactive Windows desktop and an administrator-approved loopback exemption during local testing.

The residual CDP risk, required temporary loopback exemption, and limits of Edge's `file://` blocking remain explicit. Never use a user's real browser profile or authenticated service for tests.
