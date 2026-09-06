param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$Artifacts)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$fixture = & (Join-Path $project 'tools/create_test_repo.ps1') -Large
$root = Join-Path $Artifacts ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$client = Join-Path $project 'tools/app_command.ps1'
function Invoke-App([string]$Verb,[string[]]$Values=@()) { & $client -Directory $root -Command $Verb -Arguments $Values }
function Check($Condition,[string]$Message) {if(-not $Condition) {throw $Message}}
function Wait-Idle {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $state = Invoke-App 'state'
        if(-not $state.loading) {return $state}
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Repository load did not finish.'
}
function Capture([string]$Name) {
    if ($env:GFD_SKIP_SCREENSHOTS -eq '1') {return}
    $state = Invoke-App 'screenshot' @($Name)
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root $Name))
    Check ($bytes.Length -gt 1000 -and $bytes[0] -eq 137 -and $bytes[1] -eq 80) 'Valid non-empty PNG'
    $width = $bytes[16]*16777216 + $bytes[17]*65536 + $bytes[18]*256 + $bytes[19]
    $height = $bytes[20]*16777216 + $bytes[21]*65536 + $bytes[22]*256 + $bytes[23]
    Check ($width -eq $state.width -and $height -eq $state.height) 'PNG must match full client dimensions'
}
$nested = Join-Path $fixture 'nested/deep'
New-Item -ItemType Directory -Path $nested -Force | Out-Null
$process = Start-Process -FilePath $Executable -WorkingDirectory $nested -ArgumentList ('--automation-dir "'+$root+'"') -WindowStyle Hidden -PassThru
try {
    $state=Wait-Idle
    Check ($state.source -eq 1 -and $state.files.Count -eq 1 -and $state.selectedFile -eq 'notes.txt') 'Default Unstaged'
    Check (($state.repository -replace '/', '\') -eq $fixture) 'Discover repository from nested working directory'
    Check (@($state.controls | Where-Object {$_.id -eq 'open'}).Count -eq 0) 'No Open button'
    $hints = $state.controls | Where-Object {$_.id -eq 'status'}
    Check ($hints.visible -and $hints.height -gt 0 -and $hints.y + $hints.height -le $state.height) 'Shortcut hints are visible inside the window'
    Check ($hints.text.Contains('Ctrl+Down/Up') -and $hints.text.Contains('Space Commit message')) 'File and commit message shortcuts are documented in the footer'
    # Force a response sharing violation and verify retry without replaying zoom.
    $locked = [IO.File]::Open((Join-Path $root 'response.json'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $retryId = [guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText((Join-Path $root 'request.tmp'),"$retryId`nzoom`n1`n",[Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath (Join-Path $root 'request.tmp') -Destination (Join-Path $root 'request.txt')
        Start-Sleep -Milliseconds 500
    } finally {$locked.Dispose()}
    $retryDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 100
        $reply = [IO.File]::ReadAllText((Join-Path $root 'response.json')) | ConvertFrom-Json
    } while($reply.id -ne $retryId -and [DateTime]::UtcNow -lt $retryDeadline)
    Check ($reply.id -eq $retryId -and $reply.ok -and $reply.state.fontSize -eq 12) 'Response retry does not repeat the command'
    Invoke-App 'zoom' @('-1') | Out-Null
    Invoke-App 'source' @('staged') | Out-Null
    $state=Wait-Idle
    Check ($state.files.Count -eq 9 -and $state.selectedFile -eq 'app.cpp') 'Initial staged files'
    $next=Invoke-App 'navigate-file' @('1')
    Check ($next.selectedFile -eq $state.files[1].path) 'Next file navigation'
    Check (@($next.controls | Where-Object {$_.id -eq 'diff' -and $_.focused}).Count -eq 1) 'File navigation focuses diff'
    $theme = $next.controls | Where-Object {$_.id -eq 'theme'}
    $info = $next.controls | Where-Object {$_.id -eq 'info'}
    Check ($info.x -gt $theme.x -and $info.y -eq $theme.y) 'Repository information follows theme in the toolbar'
    $back=Invoke-App 'navigate-file' @('-1')
    Check ($back.selectedFile -eq 'app.cpp') 'Previous file navigation'
    Capture 'unified.png'
    Invoke-App 'view' @('side-by-side') | Out-Null
    Capture 'side-by-side.png'
    Invoke-App 'select-file' @('large.txt') | Out-Null
    $before=Invoke-App 'scroll' @('15000')
    $after=Invoke-App 'view' @('unified')
    Check ($after.topRow -eq $before.topRow -and $after.selectedFile -eq 'large.txt' -and -not $after.loading) 'Toggle preserves position without refresh'
    Capture 'large.png'
    $zoom=Invoke-App 'zoom' @('3')
    Check ($zoom.fontSize -eq 14 -and $zoom.topRow -eq $after.topRow -and $zoom.selectedFile -eq 'large.txt') 'Font zoom preserves position'
    Capture 'zoom.png'
    $zoom=Invoke-App 'ctrl-wheel' @('-120')
    Check ($zoom.fontSize -eq 13) 'Ctrl wheel changes font size'
    Invoke-App 'zoom' @('-2') | Out-Null
    $state=Invoke-App 'key' @('end')
    Check ($state.topRow -gt 19900) 'Keyboard navigation'
    Invoke-App 'select-file' @('Юникод файл.txt') | Out-Null
    $state=Invoke-App 'state'
    Check (@($state.visibleRows | Where-Object {$_.right.text -eq 'Привет, мир!'}).Count -eq 1) 'UTF-8 state exposes Cyrillic diff text'
    Capture 'unicode.png'
    Invoke-App 'select-file' @('binary.dat') | Out-Null
    Capture 'binary.png'
    Invoke-App 'resize' @('900','600') | Out-Null
    Capture 'resized.png'
    Invoke-App 'source' @('unstaged') | Out-Null
    $state=Wait-Idle
    Check ($state.files.Count -eq 1 -and $state.files[0].path -eq 'notes.txt') 'Unstaged source'
    Invoke-App 'source' @('ready') | Out-Null
    $state=Wait-Idle
    Check ($state.commits.Count -eq 2 -and $state.files.Count -eq 2) 'Ready to push'
    Capture 'ready.png'
    $previous=$state
    $hover=Invoke-App 'hover-commit' @('1')
    Check ($hover.previewCommit -eq 1 -and -not $hover.loading -and $hover.selectedFile -eq $previous.selectedFile) 'Hover previews without loading or changing file selection'
    Check (@($hover.visibleRows | Where-Object {$_.meta -eq 'Add second feature'}).Count -eq 1) 'Hover shows full commit message'
    Capture 'hover-message.png'
    if ($env:GFD_SKIP_SCREENSHOTS -ne '1') {Invoke-App 'screenshot-commits' @('commit-dropdown.png') | Out-Null}
    $hover=Invoke-App 'hover-commit' @('2')
    Check (@($hover.visibleRows | Where-Object {$_.meta -eq 'Add first feature'}).Count -eq 1) 'Moving between commits changes preview'
    $restored=Invoke-App 'end-hover'
    Check ($restored.previewCommit -eq -1 -and $restored.selectedFile -eq $previous.selectedFile -and $restored.topRow -eq $previous.topRow) 'Closing dropdown restores original diff'
    Invoke-App 'select-commit' @('1') | Out-Null
    $state=Wait-Idle
    Check ($state.files.Count -eq 2 -and $state.files[0].path -eq '<<Commit Message>>') 'Commit message is first list entry'
    Invoke-App 'select-file' @('<<Commit Message>>') | Out-Null
    $state=Invoke-App 'state'
    Check (@($state.visibleRows | Where-Object {$_.meta -eq 'Add second feature'}).Count -eq 1) 'Full commit message is displayed'
    Check $state.plainText 'Commit message uses plain text presentation'
    $state=Invoke-App 'view' @('side-by-side')
    Check $state.plainText 'Commit message stays plain text in side-by-side mode'
    $light=Invoke-App 'theme' @('light')
    Check (-not $light.darkTheme -and $light.plainText -and $light.selectedFile -eq $state.selectedFile -and $light.fontSize -eq $state.fontSize) 'Light theme preserves message and zoom'
    $dark=Invoke-App 'theme' @('dark')
    Check ($dark.darkTheme -and $dark.plainText) 'Dark theme preserves plain message presentation'
    Capture 'commit-message.png'
    Invoke-App 'select-file' @('second.txt') | Out-Null
    $state=Invoke-App 'state'
    Check ($state.selectedFile -eq 'second.txt') 'File index after commit message entry'
    Check (-not $state.plainText) 'Selecting a file restores diff presentation'
    $message=Invoke-App 'toggle-message'
    Check ($message.plainText -and $message.selectedFile -eq '<<Commit Message>>') 'Message toggle opens current commit'
    $returned=Invoke-App 'toggle-message'
    Check (-not $returned.plainText -and $returned.selectedFile -eq 'second.txt' -and $returned.topRow -eq $state.topRow) 'Message toggle restores file and scroll'
    $first=Invoke-App 'navigate-file' @('-1')
    Check ($first.selectedFile -eq 'second.txt') 'File navigation skips message entry at first file'
    Invoke-App 'source' @('commit') | Out-Null
    $state=Wait-Idle
    Check ($state.files[0].path -eq '<<Commit Message>>') 'Single commit mode also includes message'
    Invoke-App 'open' @((Join-Path $root 'nonexistent')) | Out-Null
    $state=Wait-Idle
    Check ($state.status -like 'Git failed*' -and $state.files.Count -eq 0) 'Invalid repository error'
    Check ($state.selectedFile -eq '' -and $state.message -like 'Unable to load changes*') 'Error state clears selection and exposes details'
    Capture 'error.png'
    $rejected=$false
    try {Invoke-App 'screenshot' @('../escape.png') | Out-Null} catch {$rejected=$true}
    Check $rejected 'Reject screenshot path traversal'
    Write-Output "Interface tests passed. Artifacts: $root"
} finally {
    if(-not $process.HasExited) {
        try {Invoke-App 'close' | Out-Null} catch {}
        if(-not $process.WaitForExit(3000)) {$process.Kill()}
    }
}
