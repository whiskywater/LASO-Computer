[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $ConfigPath,
    [string] $Executable = (Join-Path $PSScriptRoot '..\build\Debug\laso-computer.exe'),
    [switch] $PolicyDenyOnly,
    [ValidateRange(1000, 120000)] [int] $TimeoutMs = 30000
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$config = (Resolve-Path -LiteralPath $ConfigPath).Path
$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$fixturePort = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()
$fixture = Start-Job -ArgumentList $fixturePort -ScriptBlock {
    param($Port)
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    $listener.Start()
    $main = @'
<!doctype html><html><head><title>LASO Fixture</title></head><body>
<h1>LASO Browser Fixture</h1>
<label for="name">Name</label><input id="name" aria-label="Name">
<button id="submit" onclick="document.querySelector('#result').textContent='Hello '+document.querySelector('#name').value">Submit</button>
<select id="choice" aria-label="Choice"><option value="one">One</option><option value="two">Two</option></select>
<a href="/second">Second page</a><p id="result">Waiting</p></body></html>
'@
    $second = '<!doctype html><html><head><title>Second</title></head><body><h1>Second page</h1><a href="/">Home</a></body></html>'
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII, $false, 1024, $true)
            $requestLine = $reader.ReadLine()
            while (($header = $reader.ReadLine()) -ne '') { }
            $stopping = $requestLine -like 'GET /__shutdown *'
            $page = if ($stopping) { 'stopping' } elseif ($requestLine -like 'GET /second *') { $second } else { $main }
            $bytes = [Text.Encoding]::UTF8.GetBytes($page)
            $headerBytes = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: text/html; charset=utf-8`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
            $stream.Write($headerBytes, 0, $headerBytes.Length)
            $stream.Write($bytes, 0, $bytes.Length)
            if ($stopping) { $listener.Stop(); break }
        } finally { $client.Dispose() }
    }
}
$worker = $null
$stderrTask = $null
$counter = 0
function Send-WorkerRequest([hashtable] $Request) {
    $script:counter++
    $Request.request_id = "browser-accept-$script:counter"
    $wire = $Request | ConvertTo-Json -Depth 50 -Compress
    $script:worker.StandardInput.WriteLine($wire)
    $script:worker.StandardInput.Flush()
    $read = $script:worker.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMs)) { throw "worker protocol response timed out: $($Request.operation)" }
    if ([string]::IsNullOrWhiteSpace($read.Result)) { throw "worker exited during $($Request.operation)" }
    return $read.Result | ConvertFrom-Json
}
function Invoke-BrowserCapability([string] $Name, [hashtable] $Arguments, [switch] $AllowFailure) {
    $jobId = "browser-action-$script:counter"
    $submitted = Send-WorkerRequest @{ protocol_version = 1; operation = 'submit'; job_id = $jobId; payload = @{ capability = $Name; arguments = $Arguments } }
    if (-not $submitted.ok -or $submitted.state -ne 'Running') { throw "submit failed for $Name" }
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        Start-Sleep -Milliseconds 100
        $status = Send-WorkerRequest @{ protocol_version = 1; operation = 'status'; external_job_id = $submitted.external_job_id }
        if (-not $status.ok) { throw "status failed for $Name" }
    } while ($status.state -eq 'Running' -and [DateTime]::UtcNow -lt $deadline)
    if ($status.state -eq 'Running') { throw "job timed out: $Name" }
    $result = Send-WorkerRequest @{ protocol_version = 1; operation = 'result'; external_job_id = $submitted.external_job_id }
    if (-not $result.ok -or $result.state -ne 'Completed') {
        if ($AllowFailure) { return $result }
        throw "capability failed: $Name ($($result.state), $($result.error))"
    }
    return $result.payload
}

try {
    Start-Sleep -Milliseconds 400
    $start = [Diagnostics.ProcessStartInfo]::new($exe)
    $start.ArgumentList.Add('--config')
    $start.ArgumentList.Add($config)
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $worker = [Diagnostics.Process]::Start($start)
    $stderrTask = $worker.StandardError.ReadToEndAsync()

    $hello = Send-WorkerRequest @{ protocol_version = 1; operation = 'hello' }
    if (-not $hello.ok) { throw 'worker handshake failed' }
    Write-Host 'PASS worker hello and provider startup'
    $unavailable = @($hello.payload.capabilities | Where-Object { $_.name -like 'browser.*' -and $_.available -ne $true })
    if ($unavailable.Count) { throw "browser provider unavailable: $(($unavailable.name) -join ', ')" }

    if ($PolicyDenyOnly) {
        $denied = Invoke-BrowserCapability 'browser.navigate' @{ url = 'https://example.com/' } -AllowFailure
        if (-not $denied.ok -or $denied.state -ne 'Failed' -or $denied.error -ne 'capability denied') {
            throw "endpoint policy did not deny browser.navigate (ok=$($denied.ok), state=$($denied.state), error=$($denied.error))"
        }
        Write-Host 'PASS installed provider does not bypass endpoint deny policy'
        return
    }

    $url = "http://127.0.0.1:$fixturePort/"
    Invoke-BrowserCapability 'browser.navigate' @{ url = $url } | Out-Null
    Write-Host 'PASS browser.navigate local fixture'
    $snapshot = Invoke-BrowserCapability 'browser.snapshot' @{}
    $snapshotText = $snapshot.content | ForEach-Object { $_.text } | Out-String
    if ($snapshotText -notmatch 'LASO Browser Fixture' -or $snapshotText -notmatch 'Name') { throw 'fixture snapshot did not contain expected DOM text' }
    Write-Host 'PASS browser.snapshot fixture DOM'

    $directFile = Invoke-BrowserCapability 'browser.navigate' @{ url = 'file:///C:/Users/Public/laso-test-only-nonexistent.txt' } -AllowFailure
    if (-not $directFile.ok -or $directFile.state -ne 'Failed') {
        throw "direct file:// navigation was not rejected by C++ URL validator (ok=$($directFile.ok), state=$($directFile.state), error=$($directFile.error))"
    }
    Write-Host 'PASS C++ rejects direct file:// navigation'
    $nameRef = [regex]::Match($snapshotText, '(?m)textbox "Name".*\[ref=(e\d+)\]')
    $submitRef = [regex]::Match($snapshotText, '(?m)button "Submit".*\[ref=(e\d+)\]')
    $choiceRef = [regex]::Match($snapshotText, '(?m)combobox "Choice".*\[ref=(e\d+)\]')
    $secondRef = [regex]::Match($snapshotText, '(?m)link "Second page".*\[ref=(e\d+)\]')
    if (-not $nameRef.Success -or -not $submitRef.Success -or -not $choiceRef.Success -or -not $secondRef.Success) { throw 'fixture snapshot was missing an expected control reference' }
    Invoke-BrowserCapability 'browser.fill' @{ target = $nameRef.Groups[1].Value; text = 'Ada' } | Out-Null
    Invoke-BrowserCapability 'browser.click' @{ target = $submitRef.Groups[1].Value } | Out-Null
    $afterClick = Invoke-BrowserCapability 'browser.snapshot' @{}
    $afterText = $afterClick.content | ForEach-Object { $_.text } | Out-String
    if ($afterText -notmatch 'Hello Ada') { throw 'click/fill did not update fixture state' }
    Write-Host 'PASS browser.fill and browser.click state change'
    Invoke-BrowserCapability 'browser.select' @{ target = $choiceRef.Groups[1].Value; value = 'two' } | Out-Null
    $tabs = Invoke-BrowserCapability 'browser.tabs' @{ action = 'list' }
    $screenshot = Invoke-BrowserCapability 'browser.screenshot' @{}
    $images = @($screenshot.content | Where-Object { $_.type -eq 'image' }).Count
    if ($images -eq 0) { throw 'browser screenshot did not return an image' }
    Write-Host 'PASS browser.select, tabs, and screenshot'
    Invoke-BrowserCapability 'browser.click' @{ target = $secondRef.Groups[1].Value } | Out-Null
    $secondSnapshot = Invoke-BrowserCapability 'browser.snapshot' @{}
    $secondText = $secondSnapshot.content | ForEach-Object { $_.text } | Out-String
    if ($secondText -notmatch 'Second page') { throw 'fixture link navigation failed' }
    Write-Host 'PASS local link navigation'
    [pscustomobject]@{ Result = 'PASS'; Browser = 'Edge owned by LASO-Computer'; Fixture = $url; Snapshot = 'PASS'; FillClick = 'PASS'; Select = 'PASS'; Tabs = 'PASS'; Screenshot = 'PASS'; LinkNavigation = 'PASS'; TabPayload = ($tabs | ConvertTo-Json -Depth 8 -Compress) }
}
finally {
    if ($worker -and -not $worker.HasExited) {
        try { Send-WorkerRequest @{ protocol_version = 1; operation = 'shutdown' } | Out-Null } catch { }
        $worker.StandardInput.Close()
        if (-not $worker.WaitForExit(5000)) { $worker.Kill(); $worker.WaitForExit() }
    }
    if ($worker) { if ($stderrTask.IsCompleted -and $stderrTask.Result.Trim()) { "Worker stderr: $($stderrTask.Result.Trim())" }; $worker.Dispose() }
    try { Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$fixturePort/__shutdown" -TimeoutSec 2 | Out-Null } catch { }
    Stop-Job $fixture -ErrorAction SilentlyContinue
    Remove-Job $fixture -Force -ErrorAction SilentlyContinue
}
