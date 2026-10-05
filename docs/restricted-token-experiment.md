# Playwright restricted-token experiment

**Result: NETWORK SUCCESS / CONTAINMENT FAILURE.** This experiment does not change the production provider. Production remains AppContainer-based and fail-closed when loopback is unavailable.

## Motivation and variants tested

With no privileged loopback exemption, AppContainer MCP initializes and lists tools but times out when it connects to C++-owned Edge's loopback CDP endpoint. No UAC, machine ACL, firewall, or loopback change was made.

The following local Windows experiments used the same pinned MCP 0.0.83, Node 24.21.0, Edge 154.0.4258.37, managed profile, dynamic CDP endpoint, one-process provider Job Object, and 512 MiB provider process-memory limit:

1. `CreateRestrictedToken` with `WinRestrictedCodeSid` as a restricting SID, `DISABLE_MAX_PRIVILEGE`, Low Integrity, and explicit read/execute grants to the Node/package/temp directories. `IsTokenRestricted` was true, but the process exited with status `0xC0000142` before MCP initialization. Consequently the MCP filesystem access checks could not be completed. No broad Windows/system-directory ACLs were considered or applied. The exact DLL/path was not established.
2. A `DISABLE_MAX_PRIVILEGE` token at Low Integrity without a restricting SID. Node started, but the first browser attachment failed with a permission error. The direct Node probe could read the generated outside file but could not write it (`EPERM`), consistent with Low Integrity write-up restrictions rather than read confidentiality. Labeling only the unique provider temp directory Low Integrity did not resolve browser attachment.
3. A `DISABLE_MAX_PRIVILEGE` token at the caller's Medium Integrity without a restricting SID. The actual C++ → MCP → Edge path passed MCP initialization, tool listing, the startup `browser.tabs` attachment probe, and the deterministic fixture: navigate, snapshot, fill, click/state verification, select, tabs, screenshot, and link navigation. This variant is **not an acceptable security boundary**.

## Token and access results

`tests/restricted_token_probe.cpp` launches the pinned Node executable using the same `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)` model and assigns it a kill-on-close Job Object with active process limit 1 and a 512 MiB per-process memory limit. It creates a generated marker file in `%TEMP%`, outside the managed Playwright runtime, then has the Node process test read, write, and child-process creation.

Observed on this Windows host:

- `IsTokenRestricted`: **false**. Disabling privileges alone did not make Windows classify the token as restricted.
- Integrity: **Medium** (RID 8192).
- Enabled privilege: `SeChangeNotifyPrivilege` (one; retained by Windows for directory traversal).
- Managed Node runtime read: **allowed**.
- Generated outside-runtime file read: **allowed**.
- Generated outside-runtime file write: **allowed**.
- Attempted Node child process: **blocked** under the one-process Job Object; Node returned a failed spawn with null status.
- MCP/CDP and full local browser fixture: **passed** in this variant.
- Normal Edge profile: **not selected**; C++ still launches Edge with a unique managed per-run profile.

This proves that privilege stripping plus the Job Object is insufficient: the token's primary user SID still authorizes arbitrary user-file reads and writes. Low Integrity is not a confidentiality boundary, and in this experiment it also prevented the browser attach operation. A restricting SID is the relevant dual-access-check mechanism, but the tested Node process did not initialize with the runtime-only grants. Solving that would require a narrowly designed runtime provisioning model and fresh tracing; this session did not add permissions to Windows directories.

The implementation did not retain a selectable restricted-token provider mode. The manual probe is built with the Debug test targets but is not registered as a passing CTest; it is an explicit Windows diagnostic that demonstrates why this fallback was rejected:

```powershell
cmake --build build --config Debug --target laso-restricted-token-probe
.\build\Debug\laso-restricted-token-probe.exe <path-to-node.exe>
```

Replace the illustrative executable path with the configured Node runtime. The probe creates a unique generated marker under `%TEMP%` and removes it on exit, including error paths after file creation. It does not alter machine ACLs or require elevation.

## Production decision

Keep the existing AppContainer provider and its explicit ACLs, one-process Job Object, fixed typed browser tool map, C++ endpoint policy, and isolated Edge profile. Do not enable medium-integrity restricted-token mode as a workaround. Browser operations remain unavailable without an authorized AppContainer loopback path. See [the Playwright provider record](playwright-provider.md) and [security model](security.md).
