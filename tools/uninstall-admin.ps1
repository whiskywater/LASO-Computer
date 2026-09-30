#requires -Version 5.1
#requires -RunAsAdministrator
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$serviceName = 'LASOComputerBroker'
$installRoot = Join-Path $env:ProgramFiles 'LASO-Computer\Broker'
$auditRoot = Join-Path $env:ProgramData 'LASO-Computer\Broker'

Write-Host 'Removing LASO-Computer broker and only its owned service/audit state.'
Write-Host 'The fixed cleanup operation removes only the LASO Playwright AppContainer legacy loopback exemption; unrelated entries are preserved.'

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($service) {
    if ($service.Status -eq 'Stopped') {
        Start-Service -Name $serviceName
        $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
    }
    $ctl = Join-Path $installRoot 'LASOComputerBrokerCtl.exe'
    if (!(Test-Path -LiteralPath $ctl -PathType Leaf)) { throw 'Broker client is missing; service removal stopped so privileged state can be investigated.' }
    & $ctl cleanup-legacy-loopback
    if ($LASTEXITCODE -ne 0) { throw 'Broker cleanup failed; service removal stopped so state can be investigated.' }
    $service.Refresh()
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name $serviceName -Force
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
    }
} else {
    Write-Warning 'Broker service is not installed; cannot query its cleanup operation. Removing only known service files.'
}
if ($service) {
    & sc.exe delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not delete broker service registration.' }
    Start-Sleep -Milliseconds 500
}

foreach ($path in @($installRoot, $auditRoot)) {
    if (!(Test-Path -LiteralPath $path)) { continue }
    $item = Get-Item -LiteralPath $path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Refusing to remove reparse-point directory: $path" }
    $reparseChild = Get-ChildItem -LiteralPath $path -Force -Recurse | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    } | Select-Object -First 1
    if ($reparseChild) { throw "Refusing recursive removal because a descendant is a reparse point: $($reparseChild.FullName)" }
    Remove-Item -LiteralPath $path -Recurse -Force
}
Write-Host 'Broker service and its service-owned files were removed.'
