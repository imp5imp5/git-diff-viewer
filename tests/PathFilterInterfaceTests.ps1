param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$fixture = & (Join-Path $project 'tools/create_test_repo.ps1')
$session = Join-Path ([IO.Path]::GetTempPath()) ('gfd-path-filter-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $session | Out-Null
$client = Join-Path $project 'tools/app_command.ps1'
function Invoke-App([string]$Verb,[string[]]$Values=@()) { & $client -Directory $session -Command $Verb -Arguments $Values }
function Wait-Idle {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $state = Invoke-App 'state'
        if(-not $state.loading) {return $state}
    } while([DateTime]::UtcNow -lt $deadline)
    throw 'Repository load did not finish.'
}
function Remove-Fixture([string]$Path,[string]$Prefix) {
    $target = [IO.Path]::GetFullPath($Path)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if(-not $target.StartsWith($temporary,[StringComparison]::OrdinalIgnoreCase) -or
       -not (Split-Path $target -Leaf).StartsWith($Prefix)) {throw "Unsafe test cleanup path: $target"}
    Remove-Item -LiteralPath $target -Recurse -Force
}
$process = $null
try {
    $inside = Join-Path $fixture 'src/git'
    $outside = Join-Path $fixture 'src/ui'
    New-Item -ItemType Directory -Path $inside,$outside -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $inside 'inside.txt'),"base`n")
    [IO.File]::WriteAllText((Join-Path $outside 'outside.txt'),"base`n")
    & git.exe -C $fixture add src
    if($LASTEXITCODE -ne 0) {throw 'Fixture add failed.'}
    & git.exe -C $fixture commit -m 'Add scoped files' | Out-Null
    if($LASTEXITCODE -ne 0) {throw 'Fixture commit failed.'}
    [IO.File]::WriteAllText((Join-Path $inside 'inside.txt'),"changed`n")
    [IO.File]::WriteAllText((Join-Path $outside 'outside.txt'),"changed`n")
    $relative = (Split-Path $fixture -Leaf) + '\src\git'
    $process = Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $fixture -Parent) -ArgumentList ('--automation-dir "'+$session+'" "'+$relative+'"') -WindowStyle Hidden -PassThru
    $state = Wait-Idle
    if($state.source -ne 6) {throw 'A relative subdirectory must open filtered History.'}
    Invoke-App 'layout' @('panels') | Out-Null
    Invoke-App 'source' @('unstaged') | Out-Null
    $state = Wait-Idle
    $paths = @($state.explorerFiles | ForEach-Object {$_.path})
    if($paths.Count -ne 1 -or $paths[0] -ne 'src/git/inside.txt') {
        throw "Unstaged FILES escaped the path filter: $($paths -join ', ')"
    }
    Invoke-App 'close' | Out-Null
    if(-not $process.WaitForExit(5000)) {throw 'First test application did not close.'}
    $process = Start-Process -FilePath $Executable -WorkingDirectory $fixture -ArgumentList ('--automation-dir "'+$session+'" src/git') -WindowStyle Hidden -PassThru
    $state = Wait-Idle
    if($state.source -ne 6) {throw 'gfd src/git must open filtered History.'}
    Invoke-App 'layout' @('panels') | Out-Null
    Invoke-App 'source' @('unstaged') | Out-Null
    $state = Wait-Idle
    $paths = @($state.explorerFiles | ForEach-Object {$_.path})
    if($paths.Count -ne 1 -or $paths[0] -ne 'src/git/inside.txt') {
        throw "gfd src/git did not filter Unstaged FILES: $($paths -join ', ')"
    }
    Invoke-App 'close' | Out-Null
} finally {
    if($process -and -not $process.HasExited) {Stop-Process -Id $process.Id -Force}
    Remove-Fixture $session 'gfd-path-filter-'
    Remove-Fixture $fixture 'GitDiffViewer-fixture-'
}