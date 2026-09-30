#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [switch]$RequireBroker,
    [switch]$CleanupLegacyLoopback
)
$ErrorActionPreference = 'Stop'
Write-Host 'LASO-Computer unattended preflight'

$candidates = @()
if ($BuildDirectory) { $candidates += Join-Path $BuildDirectory 'Release\LASOComputerBrokerCtl.exe' }
$candidates += Join-Path $env:ProgramFiles 'LASO-Computer\Broker\LASOComputerBrokerCtl.exe'
$ctl = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (!$ctl) {
    Write-Host 'Broker: not installed'
    Write-Host 'Protocol: unavailable'
    Write-Host 'Required provisioning: none for normal build and unit-test lanes'
    Write-Host 'Playwright loopback state: UNKNOWN (broker not installed)'
    Write-Host 'Browser acceptance: not verified; current AppContainer provider needs loopback access to managed Edge'
    Write-Host 'Interactive UAC expected: NO'
    if ($RequireBroker) {
        Write-Host 'Unattended test readiness: BLOCKED (attended broker bootstrap required for privileged lanes)'
        exit 2
    }
    Write-Host 'Non-privileged build/unit lanes: READY; privileged state unavailable'
    exit 0
}

$service = Get-Service -Name 'LASOComputerBroker' -ErrorAction SilentlyContinue
if (!$service -or $service.Status -ne 'Running') {
    if (!$service) { Write-Host 'Broker: binaries available; service not installed' }
    else { Write-Host "Broker: service $($service.Status)" }
    Write-Host 'Required provisioning: none for non-privileged lanes'
    Write-Host 'Playwright loopback state: UNKNOWN (broker service unavailable)'
    Write-Host 'Browser acceptance: not verified; current AppContainer provider needs loopback access to managed Edge'
    Write-Host 'Interactive UAC expected: NO'
    if ($RequireBroker) { Write-Host 'Unattended test readiness: BLOCKED (attended bootstrap or service start required)'; exit 2 }
    Write-Host 'Non-privileged build/unit lanes: READY'
    exit 0
}

$statusText = & $ctl status
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Broker: service running but unavailable or protocol-incompatible'
    Write-Host 'Interactive UAC expected: NO'
    Write-Host 'Browser acceptance: not run by this preflight'
    if ($RequireBroker) { Write-Host 'Unattended test readiness: BLOCKED'; exit 2 }
    Write-Host 'Non-privileged build/unit lanes: READY'
    exit 0
}
$status = $statusText | ConvertFrom-Json
$expectedOperations = @('query_status', 'query_appcontainer_sid', 'query_legacy_loopback', 'cleanup_legacy_loopback')
$operationMismatch = @($status.operations | Where-Object { $_ -notin $expectedOperations }).Count -gt 0 -or
    @($expectedOperations | Where-Object { $_ -notin @($status.operations) }).Count -gt 0
if ($status.broker_version -ne 1 -or @($status.operations).Count -ne 4 -or $operationMismatch) {
    Write-Host 'Broker: protocol version or operation set mismatch (broker update pending)'
    Write-Host 'Interactive UAC expected: NO'
    Write-Host 'Browser acceptance: not run by this preflight'
    if ($RequireBroker) { Write-Host 'Unattended test readiness: BLOCKED'; exit 2 }
    Write-Host 'Non-privileged build/unit lanes: READY'
    exit 0
}
$statusText | Out-Host
$loopbackText = & $ctl legacy-loopback
if ($LASTEXITCODE -ne 0) { throw 'Broker status worked but legacy-state query failed.' }
$loopback = $loopbackText | ConvertFrom-Json
if ($loopback.result.enabled) {
    Write-Host 'Playwright loopback exemption: present; required by current AppContainer-to-Edge CDP transport.'
    if ($CleanupLegacyLoopback) {
        Write-Host 'Explicit cleanup requested; this will block the current AppContainer browser lane.'
        & $ctl cleanup-legacy-loopback | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Broker cleanup failed; no UAC was requested.' }
        $verifiedText = & $ctl legacy-loopback
        if ($LASTEXITCODE -ne 0 -or ($verifiedText | ConvertFrom-Json).result.enabled) {
            throw 'Broker cleanup could not verify removal; no UAC was requested.'
        }
        Write-Host 'Playwright loopback exemption: removed by explicit request; AppContainer browser lane is BLOCKED.'
    }
} else {
    Write-Host 'Playwright loopback exemption: absent; AppContainer browser lane is BLOCKED.'
    if ($CleanupLegacyLoopback) { Write-Host 'Explicit cleanup requested; no managed exemption was present.' }
}
Write-Host 'Broker protocol: OK'
if (-not $loopback.result.enabled -or $CleanupLegacyLoopback) {
    Write-Host 'Browser acceptance: BLOCKED until loopback is provisioned by an attended administrator decision'
} else {
    Write-Host 'Browser acceptance: loopback prerequisite present; run the Playwright acceptance lane to validate'
}
Write-Host 'Interactive UAC expected: NO'
Write-Host 'Broker preflight: PASS'
