# Third-party integrations and license audit

Audited 2026-09-28 for architecture and license planning. Projects in this table were inspected as references; their source code was not copied into LASO-Computer.

| Project | Audited upstream / license | Use here |
| --- | --- | --- |
| [Microsoft Playwright MCP](https://github.com/microsoft/playwright-mcp) | npm `@playwright/mcp` 0.0.83, Apache-2.0; its transitive Playwright packages are Apache-2.0 | Selected adapter target. Fixed newline-delimited JSON-RPC/MCP stdio operations; Node module resolution and MCP handshake now work under AppContainer, but Edge creation with the remote-debugging pipe stalls at the browser startup probe. Package files are not checked in; manifest and lockfile pin exact package version. |
| [Microsoft Playwright CLI](https://github.com/microsoft/playwright-cli) | Apache-2.0 | Compared; CLI/skills are oriented to agent-driven terminal use. MCP has a persistent structured stdio interface better suited to the endpoint adapter. |
| [browser-use](https://github.com/browser-use/browser-use) | MIT; current source requires Python | Reference only. Not used as it would add a Python runtime and duplicate Playwright browser control. |
| [Microsoft UFO / UFO²](https://github.com/microsoft/UFO) | MIT | Architectural reference for UI Automation, Win32 and COM structured Windows control. No UFO code copied or runtime used. |
| [Agent-S](https://github.com/simular-ai/Agent-S) | Apache-2.0; Python-based | Future optional visual-agent reference only. Not installed or invoked. |
| [UI-TARS Desktop](https://github.com/bytedance/UI-TARS-desktop) | Apache-2.0; desktop app and Node ecosystem | Future optional visual-agent reference only. Not installed or invoked. |
| [Open Interpreter](https://github.com/openinterpreter/openinterpreter) | Apache-2.0 | Comparable project/reference only. It does not define LASO-Computer's trust boundary and is not a dependency. |
| [nlohmann/json](https://github.com/nlohmann/json) | Vendored single header 3.11.3, MIT | Core JSON parsing dependency. See its MIT license at `third_party/nlohmann_json/LICENSE.MIT`. |

The optional `plugins/playwright/package-lock.json` pins the selected npm package and transitive graph. Running `npm ci` requires a user-installed Node/npm toolchain; Node is not needed for the core executable. Runtime support for this adapter is currently blocked and must not be advertised as working. The investigation and verified limits are recorded in [playwright-provider.md](playwright-provider.md).

Windows SDK APIs (Win32, UI Automation, WIC, COM) are linked from the installed Windows SDK and are not third-party bundled runtimes. Verify upstream versions and license files again before enabling any optional integration in a release.
