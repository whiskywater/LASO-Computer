[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ConfigPath
)
$ErrorActionPreference = 'Stop'

$config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
if ($config.version -ne 1) { throw 'Chat orchestrator policy must use config version 1.' }
if ($config.default_decision -ne 'deny') { throw 'Chat orchestrator policy must default to deny.' }

$expected = @{
    'browser.status' = 'allow'
    'window.list' = 'allow'
    'ui.inspect' = 'require_approval'
    'window.focus' = 'require_approval'
    'ui.focus' = 'require_approval'
    'ui.invoke' = 'require_approval'
    'keyboard.type' = 'require_approval'
    'keyboard.key' = 'require_approval'
}
if (@($config.capabilities.PSObject.Properties).Count -ne $expected.Count) { throw 'Chat orchestrator policy contains unexpected capability grants.' }
foreach ($name in $expected.Keys) {
    if ($config.capabilities.PSObject.Properties[$name].Value -ne $expected[$name]) {
        throw "Unexpected endpoint decision for $name."
    }
}

$dangerous = @(
    'screen.capture', 'pointer.move', 'pointer.click', 'clipboard.read', 'clipboard.write',
    'shell.execute', 'browser.navigate', 'browser.snapshot', 'browser.query', 'browser.click',
    'browser.fill', 'browser.select', 'browser.tabs', 'browser.back', 'browser.screenshot'
)
foreach ($name in $dangerous) {
    if ($config.capabilities.PSObject.Properties[$name]) { throw "Dangerous capability $name must inherit deny." }
}
if ($config.shell.allowed_executables.Count -ne 0 -or $config.shell.environment_allowlist.Count -ne 0) {
    throw 'Chat orchestrator policy must not configure shell execution.'
}
if ($config.plugins.playwright.enabled) { throw 'Chat orchestrator policy must leave Playwright disabled.' }

Write-Host 'PASS chat orchestrator endpoint decisions: allow, require_approval, and default deny'
