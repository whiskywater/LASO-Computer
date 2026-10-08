# Browser automation confinement

## Current Go behavior

The Go worker reports browser status and can open a validated HTTP(S) URL in the operating system's default browser. Managed Playwright operations (DOM snapshot/query, click, fill, select, tabs, back, and screenshot) remain unavailable. The catalog reports these capabilities unavailable so Core cannot treat their names as authorization or assume an installed browser provider is usable.

## Retired C++ provider findings

The previous C++ path owned an Edge process with a unique temporary profile, attached a pinned `@playwright/mcp` provider to that Edge process over loopback CDP, and exposed only fixed typed browser tools. It did not expose arbitrary JavaScript, arbitrary MCP tools, filesystem access, upload/download, cookies, or the user's normal browser profile. Edge and the provider were process/job-object constrained; the provider ran in a Windows AppContainer with a narrow runtime ACL.

The provider used `--allow-unrestricted-file-access` to work around a Node `realpath` failure inside AppContainer. That flag bypasses an MCP filesystem guard, so the operating-system boundary and fixed endpoint capability adapter were essential. The prior audit verified that AppContainer denied access to generated files outside its runtime and blocked child process creation under its one-process job limit.

The latest recorded clean run without a loopback exemption failed when the AppContainer provider attempted to reach Edge's loopback CDP endpoint. A diagnostic without AppContainer completed the browser fixture but could read and write files outside the managed runtime, so it was rejected as an unsafe fallback. The old privileged broker could query and remove legacy loopback exemption state; it did not provision an exemption. No exemption is enabled by this Go implementation.

## Requirements for restoring managed automation

A Go implementation must retain all of these boundaries before advertising managed browser capabilities:

- Launch and own a dedicated browser process with a unique profile; never use the user's authenticated profile.
- Run a pinned provider in an OS-enforced container with access limited to its managed runtime and output directory. Do not replace this with a medium-integrity token that retains broad same-user filesystem access.
- Keep browser networking in the browser process. Limit the provider to the local browser control channel, and do not silently add machine-wide loopback exceptions or broad network permissions.
- Expose only fixed typed operations through the local per-capability policy. Keep arbitrary JavaScript, generic MCP passthrough, filesystem tools, shell, cookies/storage, upload, and download unavailable.
- Preserve bounded frames/results, redacted audit, process-tree cleanup, and fail-closed behavior when the provider or control channel is unavailable.
- Validate the actual AppContainer-to-CDP path, outside-file denial, child-process denial, URL restrictions, and cleanup on Windows before setting any managed browser capability available.

Until those checks pass in the Go runtime, managed DOM automation stays unavailable. Default-browser URL opening is a separate, explicitly grantable capability and does not provide managed browser isolation or DOM control.
