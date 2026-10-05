[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AcceptanceExecutable,
    [Parameter(Mandatory = $true)][string]$ShippingExecutable
)
$ErrorActionPreference = 'Stop'

function Invoke-WorkerHello([string]$Executable, [bool]$ExpectAcceptanceCapabilities) {
    $configPath = Join-Path ([IO.Path]::GetTempPath()) ("laso-computer-empty-env-{0}.json" -f [guid]::NewGuid().ToString('N'))
    $config = @{
        version = 1
        default_decision = 'deny'
        approval_timeout_ms = 60000
        capabilities = @{}
        plugins = @{ playwright = @{ enabled = $false; node_executable = ''; server_entry = ''; browser_executable = '' } }
        shell = @{ allowed_executables = @(); environment_allowlist = @() }
    } | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText($configPath, $config, [Text.UTF8Encoding]::new($false))

    $worker = $null
    try {
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $Executable
        $start.Arguments = '--config "{0}"' -f $configPath
        $start.WorkingDirectory = Split-Path -Parent $Executable
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        # Core's process-worker boundary intentionally supplies no inherited environment.
        $start.EnvironmentVariables.Clear()
        $worker = [Diagnostics.Process]::Start($start)

        $worker.StandardInput.WriteLine('{"protocol_version":1,"request_id":"empty-env-hello","operation":"hello","job_id":"","external_job_id":"","payload":{}}')
        $worker.StandardInput.Flush()
        $helloTask = $worker.StandardOutput.ReadLineAsync()
        if (-not $helloTask.Wait(5000)) { throw 'Worker hello timed out with an empty environment.' }
        if ([string]::IsNullOrWhiteSpace($helloTask.Result)) { throw 'Worker exited before its hello response.' }
        $hello = $helloTask.Result | ConvertFrom-Json
        if (-not $hello.ok -or $hello.request_id -ne 'empty-env-hello') { throw 'Worker hello response was invalid.' }
        $capabilities = @($hello.metadata.capabilities)
        $hasAcceptance = $capabilities -contains 'acceptance.permission'
        if ($hasAcceptance -ne $ExpectAcceptanceCapabilities) {
            throw "Acceptance capability isolation failed for $Executable."
        }

        $worker.StandardInput.WriteLine('{"protocol_version":1,"request_id":"empty-env-shutdown","operation":"shutdown","job_id":"","external_job_id":"","payload":{}}')
        $worker.StandardInput.Flush()
        $shutdownTask = $worker.StandardOutput.ReadLineAsync()
        if (-not $shutdownTask.Wait(5000)) { throw 'Worker shutdown response timed out.' }
        $shutdown = $shutdownTask.Result | ConvertFrom-Json
        if (-not $shutdown.ok -or $shutdown.request_id -ne 'empty-env-shutdown') { throw 'Worker shutdown response was invalid.' }
        $worker.StandardInput.Close()
        if (-not $worker.WaitForExit(5000)) { throw 'Worker did not exit after graceful shutdown.' }
        if ($worker.ExitCode -ne 0) { throw "Worker exited with code $($worker.ExitCode)." }
        Write-Host "PASS empty-environment hello and graceful shutdown: $Executable"
    }
    finally {
        if ($worker) {
            if (-not $worker.HasExited) { $worker.Kill(); $worker.WaitForExit() }
            $worker.Dispose()
        }
        Remove-Item -LiteralPath $configPath -Force -ErrorAction SilentlyContinue
    }
}

Invoke-WorkerHello $AcceptanceExecutable $true
Invoke-WorkerHello $ShippingExecutable $false
