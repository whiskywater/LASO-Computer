# Remote loopback worker transport

The `--listen` mode serves the existing versioned LASO process-worker protocol
on one local TCP connection at a time. `--supervise` runs the same listener and
keeps an authenticated SSH reverse tunnel connected. Both accept only IPv4
loopback addresses. Capability authorization, bounded frames, job IDs,
cancellation, replay handling, and redacted local audit events remain in the
existing worker engine.

For status checks that do not need window titles or URLs, `browser.status`
returns only visible-browser booleans and a bounded window count. This is useful
for transport validation without storing window titles in LASO job records.

The listener does not implement its own TLS or enrollment protocol. Run it only
behind an authenticated SSH reverse tunnel. SSH provides transport encryption
and authenticates the connecting workstation. Give the workstation a
preconfigured, revocable SSH identity restricted to the one loopback remote
forward port. Do not use an unrestricted account key for an unattended worker.
Never enable `GatewayPorts` for this workflow.

## Supervised endpoint

Create this OpenSSH host alias once in the endpoint user's SSH config. Keep the
Cloudflare command and host mapping there so the LASO process never shells out
to an arbitrary command and never handles SSH credentials:

```sshconfig
Host server1300
  HostName server1300ssh.wyomingtrust.law
  User server1300
  ProxyCommand C:\Progra~2\cloudflared\cloudflared.exe access ssh --hostname %h
```

Load the approved restricted-forward key into the endpoint user's OpenSSH
agent and verify the expected host key is already trusted. Then start one
supervised process in that signed-in user's session:

```powershell
laso-computer.exe --config "$env:APPDATA\LASO\Computer\config.json" `
  --supervise --ssh-host server1300
```

The supervisor launches OpenSSH with `BatchMode`, strict host-key verification,
`ExitOnForwardFailure`, a 20-second keepalive, and a 3-failure disconnect limit.
It only requests `127.0.0.1:49191:127.0.0.1:49192`. When SSH exits or the
forward cannot be confirmed, LASO-Computer marks the endpoint degraded and
retries with a 5-second to 60-second exponential delay. The listener stays up
while SSH reconnects. `/livez` on `127.0.0.1:49193` reports process liveness;
`/healthz` reports tunnel state and returns `503` while disconnected. Neither
health route binds outside loopback. The bounded service log is stored at
`%APPDATA%\LASO\Computer\service.log`.

On Windows, `--supervise` also opens a named-pipe shutdown control for the
current user SID. The pipe has a protected DACL for that SID and rejects remote
clients. To stop the supervisor gracefully, run `laso-computer.exe --shutdown`
as the same Windows user that started it. Shutdown is accepted only when no
capability job is active; otherwise the command is rejected and the listener,
tunnel, and process keep running. Retry after the active job completes. After
an accepted request, wait for `service_stop` in `service.log` and for `/livez`
to stop responding before starting the task again. This control is separate
from the worker protocol and is not exposed on the LAN or health listener.

For startup after Windows sign-in, register the command in Task Scheduler as a
task that runs only while the endpoint user is logged on. Set the trigger to
that user's logon, enable restart on failure, and keep the task at the default
user privilege level. The interactive user session is required for desktop
capabilities. Do not configure `Run whether user is logged on or not` or run it
as `SYSTEM`.

The SSH server-side port must remain loopback-only. Core can use its existing
supervised process-worker boundary to bridge stdin/stdout to that port, for
example with a fixed `nc` executable:

```yaml
max_worker_jobs_per_worker: 1
process_workers:
  windows_computer:
    executable: /usr/bin/nc
    args: [127.0.0.1, "49191"]
    remote: true
    startup_timeout_ms: 30000
    request_timeout_ms: 60000
```

The Core service can start while the endpoint is offline. If the tunnel drops,
the process-worker pipe fails and Core records the failure; it does not replay
an ambiguous computer action. The endpoint supervisor reconnects independently.
The Computer engine's job state is in memory, so an action whose response was
lost remains uncertain and must be reconciled by the parent rather than
resubmitted automatically. A process restart does not resume an in-flight local
action.

This transport is suitable only where the SSH endpoint and remote-forward
permissions are controlled. The TCP service is intentionally authenticated by
the outer SSH tunnel rather than a second application credential; it binds only
to loopback and uses the typed LASO worker protocol, never a shell or arbitrary
command interface. Do not bind it to a non-loopback interface, publish the
forwarded port, or use an unrestricted SSH identity. Keep sensitive Computer
capabilities denied unless a parent/operator explicitly authorizes them;
`shell.execute` should remain denied for this workflow.
