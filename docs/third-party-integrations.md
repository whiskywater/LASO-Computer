# Third-party integrations and license audit

Audited 2026-09-28 for architecture and license planning. Projects in this table were inspected as references; their source code was not copied into LASO-Computer.

| Project | Audited upstream / license | Use here |
| --- | --- | --- |
| [Microsoft Playwright MCP](https://github.com/microsoft/playwright-mcp) | npm `@playwright/mcp` 0.0.83, Apache-2.0; transitive Playwright packages are Apache-2.0 | Selected adapter. Its shipped config supports `browser.cdpEndpoint`; C++ owns Edge and MCP attaches. The documented `--allow-unrestricted-file-access` option avoids Node's AppContainer DOS-volume `realpath` failure; narrow OS AppContainer ACLs, a one-process Job Object, and fixed C++ capabilities remain enforced. Earlier local Debug/Release browser acceptance passed with the prior loopback-exemption state. The exemption has been removed; on 2026-09-30, Edge/MCP startup passed but the first browser attach probe failed, with cause unresolved. Clean `npm ci`/postinstall has not been run. Package files are not checked in; manifest and lockfile pin the exact version. |
| [Microsoft Playwright CLI](https://github.com/microsoft/playwright-cli) | Apache-2.0 | Compared; CLI/skills are oriented to agent-driven terminal use. MCP has a persistent structured stdio interface better suited to the endpoint adapter. |
| [browser-use](https://github.com/browser-use/browser-use) | MIT; current source requires Python | Reference only. Not used as it would add a Python runtime and duplicate Playwright browser control. |
| [Microsoft UFO / UFO²](https://github.com/microsoft/UFO) | MIT | Architectural reference for UI Automation, Win32 and COM structured Windows control. No UFO code copied or runtime used. |
| [Agent-S](https://github.com/simular-ai/Agent-S) | Apache-2.0; Python-based | Future optional visual-agent reference only. Not installed or invoked. |
| [UI-TARS Desktop](https://github.com/bytedance/UI-TARS-desktop) | Apache-2.0; desktop app and Node ecosystem | Future optional visual-agent reference only. Not installed or invoked. |
| [Open Interpreter](https://github.com/openinterpreter/openinterpreter) | Apache-2.0 | Comparable project/reference only. It does not define LASO-Computer's trust boundary and is not a dependency. |
| [nlohmann/json](https://github.com/nlohmann/json) | Vendored single header 3.11.3, MIT | Core JSON parsing dependency. See its MIT license at `third_party/nlohmann_json/LICENSE.MIT`. |

The optional `plugins/playwright/package-lock.json` pins the selected npm package and transitive graph. Running `npm ci` requires a user-installed Node/npm toolchain; Node is not needed for the core executable. Browser operations previously passed locally with Node 24.21.0; the original `realpath` failure reproduced on Node 22.23.3 as well. The exact-version named-pipe patch is wired to npm `postinstall`, but clean `npm ci` execution remains unverified. The temporary loopback exemption was removed; the latest attachment recheck is blocked. See [playwright-provider.md](playwright-provider.md) for results and residual risk.

Windows SDK APIs (Win32, UI Automation, WIC, COM) are linked from the installed Windows SDK and are not third-party bundled runtimes. Verify upstream versions and license files again before enabling any optional integration in a release.
