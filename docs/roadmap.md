# Roadmap and verified limitations

This list separates implemented Windows behavior from blocked integrations. Build and desktop results must not be read as proof of current LASO interoperability or browser support.

## Implemented in the current source

- Native C++20 Windows executable, CMake/MSVC build, Debug/Release targets.
- Deny-by-default endpoint policy and structured provider registry.
- Bounded JSONL worker transport, job status/result/cancel lifecycle, request validation, and job-state vs protocol-operation semantics.
- Redacted audit events and random persistent endpoint identity.
- Win32 screen capture, pointer and keyboard input, Unicode clipboard, visible window list/focus.
- Bounded Windows UI Automation tree inspection and uniquely matched control invocation/value setting.
- Direct restricted process execution with exact allowlist, explicit environment, bounded output, timeout/cancellation.
- Playwright MCP adapter design and fail-closed AppContainer launch. Browser actions are unavailable at present.

## Blocked acceptance items

1. **LASO worker integration:** The inspected public LASO `main` commit `ef072429c96eb9b0964e56a561591a37ccb68fbd` did not contain a worker-process v1 contract or approval/request framing. The worker's protocol unit tests are not a substitute for integration. Repeat startup, submit, polling, failures, cancellation, endpoint policy, approval, and malformed/oversized frame acceptance after LASO publishes the contract.
2. **Playwright:** The AppContainer blocks Node's attempt to inspect the `C:\` volume root. The adapter remains unavailable; no browser operation is validated. Resolve this without broadening filesystem access, then run launch/navigation/snapshot/query/click/fill/tabs/screenshot/timeout/cancel/crash/malformed-output tests.
3. **Desktop automation stability:** The purpose-built interactive fixture has passed once but a later rerun intermittently failed to deliver keyboard text into the fixture control. Investigate foreground focus behavior and rerun repeatedly before calling keyboard input reliable.
4. **Endpoint approvals:** No unsolicited request frame is sent. `require_approval` fails closed until a documented and proven correlated exchange is supported.
5. **Remote endpoint:** No authenticated remote connection, enrollment, leases, reconnect, endpoint targeting, or server-mediated revocation. LASO must spawn the worker locally.

## Not yet implemented

- Agent-S or UI-TARS visual fallback. The provider interface is the future integration point; no external visual agent has unrestricted OS access.
- Dynamic DLL plugins, install/sign/update lifecycle, or local pause/revoke UI.
- Filesystem transfer and production packaging/signing.

Do not mark the release production-ready until the integration blockers are cleared and the results are recorded against exact upstream commits.

## Migration note

The Go source remains present as reference code until the blocked acceptance lanes above are resolved. It is not part of the CMake build or the Windows CI workflow. Remove it in a follow-up after the matching LASO contract, approval framing, browser sandbox, and interactive validation have been proven.
