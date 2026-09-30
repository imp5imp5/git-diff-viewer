param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class GfdErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode); }'
[GfdErrorMode]::SetErrorMode(0x8002) | Out-Null
$project = Split-Path $PSScriptRoot -Parent
$fixture = & (Join-Path $project 'tools/create_test_repo.ps1')
$session = Join-Path ([IO.Path]::GetTempPath()) ('gfd-gaps-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $session | Out-Null
$client = Join-Path $project 'tools/app_command.ps1'
function App([string]$Command,[string[]]$Arguments=@()) {
    try { & $client -Directory $session -Command $Command -Arguments $Arguments } catch {
        if($script:process -and $script:process.HasExited) {
            $report = Join-Path $session 'crash.txt'
            $detail = if(Test-Path $report) {Get-Content $report -Raw} else {'No crash report.'}
            throw "gfd.exe exited with code $($script:process.ExitCode) during $Command. $detail"
        }
        throw
    }
}
function Idle {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $state = App 'state'
        if(-not $state.loading) {return $state}
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Repository load did not finish.'
}
function Check($Condition,[string]$Message) {if(-not $Condition) {throw $Message}}
$process = $null
try {
    $process = Start-Process -FilePath $Executable -ArgumentList ('"'+$fixture+'" --automation-dir "'+$session+'"') -WindowStyle Hidden -PassThru
    $state = Idle
    App 'source' @('staged') | Out-Null
    $state = Idle
    Check ($state.selectedFile -eq 'app.cpp' -and $state.gaps.Count -ge 3) 'Compact diff must expose omitted ranges.'
    Check (-not $state.gaps[0].up -and $state.gaps[0].down -and
           $state.gaps[-1].up -and -not $state.gaps[-1].down) 'File-edge gaps must expose only useful direction buttons.'
    $initial = $state.rowCount
    $topRemaining = $state.gaps[0].remaining
    Check ($state.hunkHeaderRows.Count -eq 0) 'Compact diff must not show hunk headers.'
    $state = App 'expand-gap' @('0','top')
    Check (-not $state.loading -and $state.rowCount -eq $initial) 'Hidden top-edge button must not activate.'
    $state = App 'double-click-gap' @('0','bottom')
    Check ($state.loading) 'First expansion must request the selected file context.'
    $state = Idle
    Check ($state.gaps[0].remaining -eq $topRemaining - 20 -and
           $state.hunkHeaderRows.Count -eq 0) 'A double click must reveal twenty lines without hunk headers.'
    $state = App 'expand-gap' @('0','all')
    Check (@($state.gaps | Where-Object {$_.id -eq 0}).Count -eq 0) 'All action must reveal the rest of the range.'
    $state = App 'view' @('side-by-side')
    Check (@($state.gaps | Where-Object {$_.id -eq 0}).Count -eq 0) 'Expansion must survive side-by-side view.'
    Check ($state.hunkHeaderRows.Count -eq 0) 'Side-by-side diff must not show hunk headers.'
    $state = App 'expand-gap' @('1','bottom')
    Check ($state.hunkHeaderRows.Count -eq 0) 'Side-by-side expanded context must not show hunk headers.'
    $tail = $state.gaps[-1]
    $state = App 'expand-gap' @([string]$tail.id,'bottom')
    Check (-not $state.loading -and $state.gaps[-1].remaining -eq $tail.remaining) 'Hidden bottom-edge button must not activate.'
    $state = App 'double-click-gap' @([string]$tail.id,'top')
    Check ($state.gaps[-1].remaining -eq $tail.remaining - 20) 'A cached range must also handle both clicks at the file end.'
    App 'full-file' @('on') | Out-Null
    $state = Idle
    Check ($state.fullFile -and $state.hunkHeaderRows.Count -eq 0) 'Full-file side-by-side diff must not show hunk headers.'
    $state = App 'view' @('unified')
    Check ($state.hunkHeaderRows.Count -eq 0) 'Full-file unified diff must not show hunk headers.'
    App 'close' | Out-Null
    Check ($process.WaitForExit(5000)) 'Application did not close.'
} finally {
    if($process -and -not $process.HasExited) {Stop-Process -Id $process.Id -Force}
    foreach($entry in @(@($session,'gfd-gaps-'),@($fixture,'GitDiffViewer-fixture-'))) {
        $target = [IO.Path]::GetFullPath($entry[0])
        $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if(-not $target.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or
           -not (Split-Path $target -Leaf).StartsWith($entry[1])) {throw "Unsafe test cleanup path: $target"}
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}
