# Chat orchestrator endpoint policy

Use `chat-orchestrator-v1.json` as an explicit LASO-Computer endpoint configuration for a local chat-orchestrator worker. It follows default deny. Status is allowed. Window discovery returns only visible supported-browser windows whose active UI Automation document reports an `https://chatgpt.com` URL, with a generic label and an active-window flag; conversation titles are neither returned nor trusted for identity. UI Automation inspection, window/control focus, control invocation, and keyboard input require a correlated Core approval. Inspection is approval-gated because accessible names can include visible page content. Keyboard input also fails closed unless the foreground window is a ChatGPT browser. Approval is fail-closed if Core is disconnected or the response is absent, denied, expired, or malformed.

## Canonical tool arguments

| Tool | Accepted arguments |
| --- | --- |
| `browser.status` | `{}` |
| `window.list` | `{}` |
| `window.focus` | Required non-empty `window_id`, at most 256 bytes |
| `ui.inspect` | Optional non-empty `window_id`, at most 256 bytes; omitted means foreground ChatGPT window |
| `ui.focus` | Required non-empty `window_id` (256 bytes max) and `target` (512 bytes max) |
| `ui.invoke` | Required non-empty `window_id` (256 bytes max), `target` (512 bytes max), and `action` in `click`, `double_click`, `submit` |
| `keyboard.type` | Required non-empty `text`, at most 4096 bytes |
| `keyboard.key` | Required `key`: `ENTER`, `ESC`, `TAB`, `SPACE`, `BACKSPACE`, `DELETE`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `HOME`, or `END` |

`window.list` reports at most 32 visible ChatGPT browser windows and replaces each window title with the constant `ChatGPT`. `ui.inspect` prefers the active web document, visits at most 4096 control nodes to depth 16, and returns at most 128 visible named controls, with each name capped at 256 UTF-16 characters and automation ID at 128. UIA target resolution ignores disabled/off-screen duplicates; focus additionally requires keyboard-focusability, and `click`/`submit` require the Invoke pattern. `double_click` resolves one unique target and derives its rectangle from UI Automation. The provider requires the target to be enabled, on-screen, non-empty, fully contained by the foreground ChatGPT window, and within virtual desktop bounds; it then sends exactly two left-button clicks at the rectangle center after checking cancellation and foreground identity. The request schema accepts no coordinates. Inspection does not return clipboard values or capture the whole desktop.

`screen.capture`, pointer actions, clipboard access, shell execution, and Playwright browser actions remain denied by the default. Do not merge this example into a user's existing configuration without reviewing and backing up that configuration. Pass the example explicitly with `--config` for an isolated endpoint process.
