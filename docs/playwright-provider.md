# Playwright provider investigation (2026-09-29)

## Current outcome: blocked

The C++ provider does not currently provide working browser automation. It fails closed and marks the Playwright adapter unhealthy unless a real browser operation succeeds during provider startup. Do not enable its capabilities expecting them to work.

Tested on Windows 11 x64 with MSVC/VS 2022, Node.js 24.21.0 x64, `@playwright/mcp` 0.0.83, its reported server build `1.64.0-alpha-1790635538000`, and the installed Edge executable at `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe` (Edge 154.0.4258.37 / Chromium 154.0.8037.58).

## Findings

1. The original exact failure was Node's CommonJS main-module resolver calling `fs.realpathSync`/`lstat` on `C:\`. The AppContainer correctly denied this with `EPERM`. Adding `--preserve-symlinks` alone did not fix the main entrypoint realpath pass. `--preserve-symlinks-main` did: Node reaches MCP initialization without granting any permission on the volume root.
2. The adapter initially used `Content-Length` frames. The pinned Playwright MCP stdio transport uses newline-delimited JSON. The C++ transport now bounds the complete JSONL line centrally on both reads and writes, correlates JSON-RPC ids, handles notifications/server requests, and validates the pinned server/tool handshake.
3. MCP initialize and `tools/list` both complete under the AppContainer. This alone was a false readiness signal because Playwright lazily starts the browser.
4. A real `browser_tabs` startup probe reaches Playwright's Edge launch. The child trace showed the browser command includes the explicit Edge executable, the isolated LASO-managed profile, and `--remote-debugging-pipe`. Node then stalls synchronously inside `child_process.spawn` before returning a child handle or emitting a spawn/error event. The bounded 10-second startup probe times out, shuts down the process job, revokes the AppContainer ACL entries, removes the unique per-run temp/profile tree, and leaves the provider unavailable.
5. Edge launched under the same AppContainer with a simple diagnostic command and `--no-sandbox`, but that did not demonstrate a working Playwright MCP pipe/browser session. The flag is not retained: disabling Chromium's sandbox without proving the actual integration would be an unjustified weakening. Edge with its normal Chromium sandbox also crashed in a minimal AppContainer launch probe. The exact Edge/Node interaction still needs tracing at the Windows process-creation boundary.

## Isolation currently retained

- Node runs directly as a child process; no shell is involved.
- Its environment is explicitly constructed and does not inherit arbitrary user variables or credentials.
- The child receives only its three standard handles; a kill-on-close Job Object owns the process tree.
- The process is in the `LASOComputer.Playwright` AppContainer with the Internet Client capability. No inbound listener or local-network capability is added.
- The AppContainer gets read/execute ACLs on the configured Node executable directory and pinned package `node_modules` tree. It gets read/write/execute/delete access only to a unique per-run directory below `%LOCALAPPDATA%\LASO-Computer\plugin-temp`, containing its provider config, output, and browser profile.
- The profile is explicitly selected inside that unique directory. It is not the user's Chrome/Edge profile and has no cookies, passwords, history, or extensions from that profile. Shutdown revokes the explicit AppContainer ACEs and removes the per-run directory.
- The installed Edge tree already grants `ALL APPLICATION PACKAGES` read/execute access. LASO-Computer does not alter its ACL. No access is granted to `C:\`, the user's documents, or the general user profile.
- Only fixed structured MCP operations are mapped. Generic `tools/call`, arbitrary JavaScript, filesystem access, uploads, downloads, cookies, and storage are not exposed.

ACL lifecycle was checked with `icacls`: the explicit AppContainer SID grants on the Node/package and temp trees disappear after provider shutdown. The per-run directory is removed after timeout. Older diagnostic runs made before per-run temp cleanup left files in the local test machine's dedicated AppContainer temp cache; they are not project files and are not included in this repository.

To repeat the startup acceptance check after provisioning the pinned Node and MCP package, use a local config with Playwright enabled and an explicit Edge executable, then run:

```powershell
.\scripts\test-playwright-startup.ps1 `
  -ConfigPath "$env:LOCALAPPDATA\LASO-Computer\config.json" `
  -Executable .\build\Debug\laso-computer.exe
```

The script returns a nonzero result unless the worker hello reports every browser capability available. On this machine it currently reports all nine capabilities unavailable after the browser startup probe times out; that is the expected failing acceptance result, not a pass for browser functionality.

## Provider choice

MCP remains the selected integration. Its persistent structured JSON-RPC operation surface fits the capability adapter and policy mapping. The Playwright CLI is oriented around agent/terminal-driven command workflows and would still need the same Windows browser process launch; changing front ends does not currently address the observed `CreateProcess`/remote-debug-pipe stall. This is a design comparison, not a Windows CLI acceptance run.

## Validation matrix

| Validation | Result |
| --- | --- |
| Node main-entry resolution without `C:\` access | PASS |
| MCP initialize and server version handshake | PASS |
| Fixed MCP tools list validation | PASS |
| Startup probe fails closed when browser cannot launch | PASS (Debug and Release worker hello report browser capabilities unavailable) |
| `scripts/test-playwright-startup.ps1` acceptance check | BLOCKED (correctly returns nonzero; all nine browser capabilities are unavailable) |
| Node/package/temp AppContainer ACL revocation | PASS (`icacls` confirmed the provider SID ACEs were removed after shutdown) |
| Per-run provider/profile directory cleanup after timeout | PASS (managed per-run directory removed; legacy artifacts from earlier pre-fix probes remain only in the local AppContainer cache) |
| Edge launch under AppContainer through actual MCP remote-debugging pipe | BLOCKED (synchronous `child_process.spawn` stall; 10-second probe timeout) |
| Navigate, snapshot/query, click, fill, select, tabs, screenshot | BLOCKED (browser does not launch) |
| Browser cancellation/crash/restart and external-file denial tests | NOT RUN (provider cannot launch browser) |
| Public example.com through LASO-Computer | BLOCKED before navigation |
| Local HTTP fixture / loopback | NOT RUN |

| C++ unit suite | PASS (fresh Debug and Release CTest, 1/1 each) |
| Interactive Windows desktop fixture | PASS (fresh Debug and Release fixture runs; keyboard/click and clipboard assertions were skipped when Windows withheld foreground focus/usable clipboard access) |

These core/desktop passes are not a Playwright acceptance pass. Do not close issue #2 or mark Playwright PASS until browser operations and restricted-resource denial tests succeed on the final process model.
