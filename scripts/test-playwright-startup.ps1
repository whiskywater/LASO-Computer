[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ConfigPath,
    [string] $Executable = (Join-Path $PSScriptRoot '..\build\Debug\laso-computer.exe'),
    [ValidateRange(1000, 120000)]
    [int] $StartupTimeoutMs = 45000
)

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$configFullPath = (Resolve-Path -LiteralPath $ConfigPath).Path
$startInfo = [Diagnostics.ProcessStartInfo]::new($executablePath)
$startInfo.ArgumentList.Add('--config')
$startInfo.ArgumentList.Add($configFullPath)
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$worker = [Diagnostics.Process]::Start($startInfo)
$errorTask = $worker.StandardError.ReadToEndAsync()

try {
    $hello = @{
        protocol_version = 1
        request_id = 'playwright-startup-smoke'
        operation = 'hello'
    } | ConvertTo-Json -Compress
    $worker.StandardInput.WriteLine($hello)
    $worker.StandardInput.Flush()

    $responseTask = $worker.StandardOutput.ReadLineAsync()
    if (-not $responseTask.Wait($StartupTimeoutMs)) {
        throw "LASO-Computer worker did not answer hello within $StartupTimeoutMs ms"
    }
    $line = $responseTask.Result
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw 'LASO-Computer worker exited before returning hello'
    }
    $response = $line | ConvertFrom-Json
    if ($response.state -ne 'Completed' -or $response.ok -ne $true) {
        throw "LASO-Computer hello failed: $line"
    }

    $browserCapabilities = @($response.payload.capabilities | Where-Object { $_.name -like 'browser.*' })
    if ($browserCapabilities.Count -eq 0) {
        throw 'Worker did not report the browser capability set'
    }
    $unavailable = @($browserCapabilities | Where-Object { $_.available -ne $true })
    if ($unavailable.Count -gt 0) {
        $names = ($unavailable | ForEach-Object name) -join ', '
        throw "Playwright browser startup failed closed; unavailable: $names"
    }

    'Playwright browser startup probe passed; browser capabilities are available.'
}
finally {
    if ($worker -and -not $worker.HasExited) {
        $worker.StandardInput.Close()
        if (-not $worker.WaitForExit(5000)) {
            $worker.Kill()
            $worker.WaitForExit()
        }
    }
    if ($worker) {
        if ($errorTask.IsCompleted) {
            $stderr = $errorTask.Result.Trim()
            if ($stderr) { "Worker stderr: $stderr" }
        }
        $worker.Dispose()
    }
}
