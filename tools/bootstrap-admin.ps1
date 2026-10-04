#requires -Version 5.1
#requires -RunAsAdministrator
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

Write-Host 'Installing/updating the LASO-Computer privileged broker.'
Write-Host 'This operation requires an attended elevated shell; this script never self-elevates.'
Write-Host 'Privileges installed: query broker status, query LASO Playwright AppContainer SID, query and remove only its legacy loopback exemption.'
Write-Host 'No generic command, process launch, arbitrary ACL, firewall, or exemption-add operation is installed.'

$buildDirectory = Join-Path $PSScriptRoot '..\build'
$brokerSource = Join-Path $buildDirectory 'Release\LASOComputerBroker.exe'
$ctlSource = Join-Path $buildDirectory 'Release\LASOComputerBrokerCtl.exe'
if (!(Test-Path -LiteralPath $brokerSource -PathType Leaf) -or !(Test-Path -LiteralPath $ctlSource -PathType Leaf)) {
    throw 'Build the Release LASOComputerBroker and LASOComputerBrokerCtl targets in the repository build directory first.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$authorizedSid = $identity.User.Value
if (!$authorizedSid.StartsWith('S-1-5-21-', [StringComparison]::Ordinal)) {
    throw 'Run bootstrap under the same local/domain user account that will call the broker.'
}

$installRoot = Join-Path $env:ProgramFiles 'LASO-Computer\Broker'
$auditRoot = Join-Path $env:ProgramData 'LASO-Computer\Broker'
foreach ($path in @($installRoot, $auditRoot)) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Refusing reparse-point directory: $path" }
    } else {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
}

$serviceName = 'LASOComputerBroker'
$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($service) {
    if ($service.Status -ne 'Stopped') { Stop-Service -Name $serviceName -Force; $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) }
}

$brokerExe = Join-Path $installRoot 'LASOComputerBroker.exe'
$ctlExe = Join-Path $installRoot 'LASOComputerBrokerCtl.exe'
foreach ($path in @($brokerExe, $ctlExe)) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Refusing reparse-point file: $path" }
    }
}
Copy-Item -LiteralPath $brokerSource -Destination $brokerExe -Force
Copy-Item -LiteralPath $ctlSource -Destination $ctlExe -Force

$systemSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
$adminsSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
$usersSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
$dirAcl = New-Object Security.AccessControl.DirectorySecurity
$dirAcl.SetAccessRuleProtection($true, $false)
$inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit
foreach ($entry in @(@($systemSid, 'FullControl'), @($adminsSid, 'FullControl'), @($usersSid, 'ReadAndExecute'))) {
    $rights = [Enum]::Parse([Security.AccessControl.FileSystemRights], [string]$entry[1])
    $rule = [Security.AccessControl.FileSystemAccessRule]::new($entry[0], $rights, $inheritance,
        [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
    $null = $dirAcl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $installRoot -AclObject $dirAcl

$auditAcl = New-Object Security.AccessControl.DirectorySecurity
$auditAcl.SetAccessRuleProtection($true, $false)
foreach ($entry in @(@($systemSid, 'FullControl'), @($adminsSid, 'FullControl'))) {
    $rights = [Enum]::Parse([Security.AccessControl.FileSystemRights], [string]$entry[1])
    $rule = [Security.AccessControl.FileSystemAccessRule]::new($entry[0], $rights, $inheritance,
        [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
    $null = $auditAcl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $auditRoot -AclObject $auditAcl

$binaryPath = '"{0}" --service --authorized-sid={1}' -f $brokerExe, $authorizedSid
if (!$service) {
    New-Service -Name $serviceName -DisplayName 'LASO-Computer Privileged Broker' -Description 'Narrow typed provisioning broker for LASO-Computer.' -BinaryPathName $binaryPath -StartupType Automatic | Out-Null
}
& sc.exe config $serviceName binPath= $binaryPath start= auto obj= LocalSystem | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not configure the broker service.' }
& sc.exe sidtype $serviceName unrestricted | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not enable the broker service SID.' }
# Restrict control to SYSTEM and Administrators. The current interactive user
# receives only the typed pipe operations through the pipe DACL.
& sc.exe sdset $serviceName 'D:(A;;GA;;;SY)(A;;GA;;;BA)' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not restrict broker service control permissions.' }
& sc.exe failure $serviceName reset= 86400 actions= restart/5000/restart/15000/none/0 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not configure broker recovery.' }
Start-Service -Name $serviceName
(Get-Service -Name $serviceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
$status = & $ctlExe status
if ($LASTEXITCODE -ne 0) { throw 'Broker installed but its named-pipe status check failed.' }
Write-Host $status
Write-Host "Installed broker for caller SID $authorizedSid. No loopback exemption was added."
