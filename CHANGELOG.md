# Changelog

## Unreleased

- Added an optional typed Windows service broker and unattended development preflight. The broker exposes only status, fixed Playwright AppContainer SID lookup, and inspection/removal of that SID's legacy loopback exemption; it provides no generic command or process execution.
- Bounded worker request replay protection as a rolling 4,096-ID window; capped active jobs at 64, retained completed results up to 256, and kept job-ID replay protection bounded while preventing IDs for retained jobs from being reused.
- Enabled the configured Job Object process-memory limits for MCP and Edge.
- Removed the CDP port reservation race by launching Edge with port 0 and validating its `DevToolsActivePort`, browser WebSocket identity, and listener PID before using the endpoint.
- Added regression coverage for long worker lifetimes and strict broker RPC validation.

The broker service has not been installed or lifecycle-tested in an elevated session. The unattended preflight and non-privileged tests run without UAC. The browser fixture has been attempted after loopback-exemption cleanup; see [Playwright provider status](docs/playwright-provider.md) for the current result.
