# Roadmap and verified limitations

This list separates locally validated Windows behavior from unresolved publication/integration lanes. Browser tests are local acceptance evidence, not proof of current LASO interoperability or production readiness.

## Implemented in the current source

- Native C++20 Windows executable, CMake/MSVC build, Debug/Release targets.
- Deny-by-default endpoint policy and structured provider registry.
- Bounded JSONL worker transport, job status/result/cancel lifecycle, request validation, and job-state vs protocol-operation semantics.
- Redacted audit events and random persistent endpoint identity.
- Win32 screen capture, pointer and keyboard input, Unicode clipboard, visible window list/focus.
- Bounded Windows UI Automation tree inspection and uniquely matched control invocation/value setting.
- Direct restricted process execution with exact allowlist, explicit environment, bounded output, timeout/cancellation.
- Playwright MCP 0.0.83 adapter with bounded JSON-RPC transport, fixed tool map, AppContainer/Job Object, C++-owned Edge, isolated profile, loopback CDP, and structured browser operations. The Node `realpath` root cause is Windows AppContainer DOS-volume path resolution; no broad filesystem ACE was added. The documented MCP unrestricted-file option is used while the OS AppContainer remains restrictive. Generated outside-file read/write and child process tests were denied. See `playwright-provider.md` for the exact tradeoff and evidence.
- Optional privileged Windows broker, strict typed RPC protocol/client, attended bootstrap/uninstall scripts, and no-UAC preflight. The service binaries compile and RPC policy tests pass without elevation; installing/restarting the service remains an attended bootstrap lane.

## Blocked acceptance items

1. **LASO worker integration:** The inspected public LASO `main` commit `ef072429c96eb9b0964e56a561591a37ccb68fbd` did not contain a worker-process v1 contract or approval/request framing. The worker's protocol unit tests are not a substitute for integration. Repeat startup, submit, polling, failures, cancellation, endpoint policy, approval, and malformed/oversized frame acceptance after LASO publishes the contract.
2. **Playwright cleanup/provisioning:** A clean Debug reproduction on 2026-09-30 launched Edge, validated `DevToolsActivePort`/browser identity/listener PID, and completed MCP initialize/tool listing; the first AppContainer MCP browser attach timed out at `127.0.0.1:<dynamic-port>`. `CheckNetIsolation` showed no loopback exemption. Running the same C++-owned Edge with the same dynamic endpoint, MCP 0.0.83 and Job Object limits outside AppContainer passed the full deterministic fixture. The endpoint and port-0 selection are therefore not the regression; AppContainer loopback isolation is. No exemption was restored and no UAC was requested. Browser operations remain BLOCKED in the production AppContainer until an attended security decision authorizes the narrow machine loopback state or a non-TCP transport is implemented. Prior AppContainer outside-file and process-launch denial evidence remains. Clean `npm ci`/postinstall and hosted browser acceptance are not verified. The MCP option disables its own file-root guard, so preserve the exact OS ACL and fixed C++ capability map. Same-user CDP hijacking remains possible. See [the provider record](playwright-provider.md).

   A 2026-09-30 medium-integrity, privilege-stripped `CreateRestrictedToken` diagnostic passed the full local fixture, but it was not classified as restricted (`IsTokenRestricted=false`) and could read/write a generated file outside its runtime. A one-process Job Object still blocked its child-process attempt. A `WinRestrictedCodeSid` token failed Node initialization (`0xC0000142`) with runtime-scoped ACL grants. No restricted-token fallback is shipped; see [the experiment record](restricted-token-experiment.md).
3. **Desktop input/clipboard:** Debug and Release disposable-window smoke tests passed for the checks that Windows allowed. Keyboard/click operations were skipped because the foreground fixture did not receive focus; clipboard read/write was skipped because the clipboard was unavailable or held non-text data. Re-run those in a suitable interactive desktop session before claiming those paths validated.
4. **Endpoint approvals:** No unsolicited request frame is sent. `require_approval` fails closed until a documented and proven correlated exchange is supported.
5. **Remote endpoint:** No authenticated remote connection, enrollment, leases, reconnect, endpoint targeting, or server-mediated revocation. LASO must spawn the worker locally.

## Not yet implemented

- Agent-S or UI-TARS visual fallback. The provider interface is the future integration point; no external visual agent has unrestricted OS access.
- Dynamic DLL plugins, install/sign/update lifecycle, or local pause/revoke UI.
- Filesystem transfer and production packaging/signing.

Do not mark the release production-ready until the LASO worker contract is validated, dependency provisioning and hosted CI are checked, and the skipped interactive input/clipboard tests pass in an appropriate session.

## Migration note

The Go source remains present as reference code until the blocked acceptance lanes above are resolved. It is not part of the CMake build or the Windows CI workflow. Remove it in a follow-up after the matching LASO contract, approval framing, browser sandbox, and interactive validation have been proven.
