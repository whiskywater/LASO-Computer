# Unattended Windows development

LASO-Computer does not automate Windows UAC. UAC is an attended consent boundary; Codex must never intentionally trigger an interactive elevation prompt during an unattended task. If a new privileged operation is needed and the installed broker does not support it, continue the non-privileged work, record the missing typed operation, and leave a bootstrap/update requirement for an attended session.

## One-time attended bootstrap

Build the broker targets with Visual Studio 2022/CMake in Release, then open an elevated PowerShell session while present and run:

```powershell
& .\tools\bootstrap-admin.ps1
```

The script does not self-elevate. It installs or updates `LASOComputerBroker` under `%ProgramFiles%\LASO-Computer\Broker`, runs it as LocalSystem with a service SID, restricts service control to SYSTEM/Administrators, authorizes only the interactive account SID captured by bootstrap at the named pipe, and applies protected directory ACLs. Run the elevated shell as the same account that will call the broker. The service and audit directory are created under `%ProgramData%\LASO-Computer\Broker`.

The service uses only local named-pipe RPC at `\\.\pipe\LASOComputerBroker`; remote pipe clients are rejected. The pipe DACL grants access to SYSTEM and the configured user SID, with client data/metadata rights but without permission to create another pipe instance. The service also impersonates the client and verifies that the pipe client is that user in a nonzero interactive session. Requests are versioned JSON, capped at 4 KiB, strictly schema checked, and dispatched to compiled functions. Overlapped connect/read/write operations have bounded waits so an authorized client cannot hold the service indefinitely. Privileged actions are audited without arguments or page content.

## Supported privileged operations

- `query_status`: broker protocol version and fixed operation list.
- `query_appcontainer_sid`: derive the fixed `LASOComputer.Playwright` AppContainer SID.
- `query_legacy_loopback`: inspect whether that exact SID appears in the machine loopback-exemption list.
- `cleanup_legacy_loopback`: remove only that SID while preserving all other entries, then verify removal.

There is no operation to add loopback exemptions, launch processes, execute shell commands, edit arbitrary ACLs/registry/firewall rules, or control arbitrary services. LocalSystem is used only because Windows exposes the loopback-exemption list as machine-wide network-isolation configuration; the callable surface is limited to the four operations above. If a compromised authorized client invokes the broker, it can learn status/SID and remove the LASO Playwright SID's loopback exemption. It cannot gain general LocalSystem execution.

Windows' `NetworkIsolationSetAppContainerConfig` replaces the whole configured SID list and offers no compare-and-swap. The broker reads the list, preserves every unrelated SID, and removes only the fixed LASO SID; a concurrent administrator change in the narrow read/write window cannot be atomically excluded. Avoid running this cleanup while another administrator is editing loopback exemptions. Do not add new list-writing operations without resolving this platform limitation.

Bootstrap never adds a loopback exemption. `dev-preflight.ps1` queries the fixed AppContainer's state through the broker and never removes it by default. Because the current browser needs loopback, removal is opt-in with `-CleanupLegacyLoopback`; the script warns that this blocks the browser lane. Cleanup remains idempotent and does not alter unrelated AppContainer entries. A 2026-09-30 clean Debug reproduction found that no-exemption browser attachment fails because AppContainer MCP gets `ETIMEDOUT` connecting to C++-owned Edge over loopback. The same dynamic CDP endpoint and complete browser fixture pass when the provider is run outside AppContainer for diagnosis. See the Playwright provider document. Browser acceptance therefore requires an attended decision about a narrowly scoped loopback allowance or a future non-TCP transport; this workflow does not request UAC or silently recreate an exemption.

## Preflight and unattended work

Run this before starting long tasks:

```powershell
.\tools\dev-preflight.ps1
```

The preflight never elevates. With an installed broker, it checks protocol compatibility and queries the fixed loopback state through typed RPC. It performs cleanup only when explicitly invoked with `-CleanupLegacyLoopback`. Without a broker, it reports that browser loopback state is unknown while allowing normal build, unit, and protocol tests to proceed. It does not perform a browser acceptance run; verify the Playwright lane separately before scheduling long browser work. On 2026-09-30, the current host's browser attach probe timed out because the AppContainer could not reach Edge's CDP listener over loopback after the exemption had been removed. The native client also supports `LASOComputerBrokerCtl.exe preflight --require-loopback` to fail closed for a browser-dependent lane. Use `-RequireBroker` only for a task whose planned lane truly depends on broker queries.

If preflight reports a broker version mismatch, do not install it during unattended work. Build the new broker and continue tests that do not depend on the new operation; report “broker update pending” for the next attended bootstrap. Adding privileged functionality requires a deliberate broker protocol/source change, review, tests, and attended update. Current AppContainer-to-Edge CDP loopback is not available with an empty loopback-exemption list; preflight reports whether legacy state is present but does not add it.

## Test classes

- **Unattended:** CMake configure/build, unit and protocol tests, policy/config tests, Playwright fixture and containment tests when the host prerequisites are validated, and broker protocol/client tests. They must not show UAC. Current Playwright attachment is blocked on this host; this preflight does not convert that failure into a pass.
- **Interactive desktop:** tests that need an unlocked foreground desktop, such as focus-sensitive `SendInput` behavior. These need a logged-in interactive session but not elevation.
- **Bootstrap/admin:** install/update/uninstall of the service, and any newly added administrator-only broker capability. These happen only when a person is present.

Service integration tests that require installing or restarting the service are bootstrap/admin tests and are not silently run by ordinary CTest.

## Update and uninstall

An upgrade may replace a service binary, so it remains an attended elevated action. Build the new Release broker binaries, then rerun the same `bootstrap-admin.ps1`; it stops the existing service, copies the fixed binaries, reapplies the service and directory policy, and verifies pipe connectivity. The script is idempotent. If a prior update installed an incompatible protocol, the non-privileged work can still proceed while the update waits for an attended session.

To remove the broker, while present use an elevated PowerShell session:

```powershell
& .\tools\uninstall-admin.ps1
```

Uninstall first invokes the fixed cleanup operation, then removes the service and its known install/audit directories. It refuses to recursively remove reparse-point roots and does not remove unrelated network-isolation entries. The scripts never disable UAC or save administrator credentials.
