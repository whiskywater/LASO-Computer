# Chat orchestrator endpoint policy

Use `chat-orchestrator-v1.json` as an explicit LASO-Computer endpoint configuration for a local chat-orchestrator worker. It follows default deny. Status is allowed. Window discovery returns only visible supported-browser windows whose title identifies ChatGPT, with a generic label and an active-window flag; conversation titles are not returned. UI Automation inspection, window/control focus, control invocation, and keyboard input require a correlated Core approval. Inspection is approval-gated because accessible names can include visible page content. Keyboard input also fails closed unless the foreground window is a ChatGPT browser. Approval is fail-closed if Core is disconnected or the response is absent, denied, expired, or malformed.

The current Core worker profile allows `ui.invoke` with a target and action but has no value field. The first profile should use `action: "invoke"`; `set_value` remains rejected without a bounded value argument. Use the approved `keyboard.type` path for text entry.

`window.list` reports at most 32 visible ChatGPT browser windows and replaces each window title with the constant `ChatGPT`. `ui.inspect` returns at most 32 control nodes at depth 8; each control name is capped at 256 characters and each automation ID at 128. It does not return values from the Windows clipboard or capture the whole desktop. Keyboard text is limited to 1024 bytes per call.

`screen.capture`, pointer actions, clipboard access, shell execution, and Playwright browser actions remain denied by the default. Do not merge this example into a user's existing configuration without reviewing and backing up that configuration. Pass the example explicitly with `--config` for an isolated endpoint process.
