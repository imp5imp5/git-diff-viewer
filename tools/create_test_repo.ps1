param([switch]$Large)
$ErrorActionPreference = 'Stop'
# Always allocate a fresh directory; never accept a user repository as the target.
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('GitDiffViewer-fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
function Git([string[]]$GitArgs) {
    & git.exe -C $fixtureRoot @GitArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Fixture git failed: $GitArgs" }
}
function Write-Fixture([string]$Name,[string]$Text) {
    [IO.File]::WriteAllText((Join-Path $fixtureRoot $Name),$Text,[Text.UTF8Encoding]::new($false))
}
Git @('init','-b','main')
Git @('config','user.name','GitDiffViewer Fixture')
Git @('config','user.email','fixture@example.invalid')
Git @('config','commit.gpgsign','false')
Git @('config','core.hooksPath','.no-hooks')
Git @('config','core.autocrlf','false')
$appLines = 1..100 | ForEach-Object { 'int value{0} = {0};' -f $_ }
Write-Fixture 'app.cpp' (($appLines -join "`n") + "`n")
Write-Fixture 'notes.txt' "A tracked working-tree file.`n"
Write-Fixture 'old name.txt' "This file will be renamed.`n"
Write-Fixture 'deleted.txt' "This file will be deleted.`n"
Git @('add','.')
Git @('commit','-m','Base application')
Git @('checkout','-b','feature/review')
Write-Fixture 'first.txt' "First local commit.`n"
Git @('add','.')
Git @('commit','-m','Add first feature')
Write-Fixture 'second.txt' "Second local commit.`n"
Git @('add','.')
Git @('commit','-m','Add second feature')
Git @('branch','--set-upstream-to=main')
Git @('branch','no-upstream')
$appLines[29] = 'int value30 = 0; // Ready for review'
$appLines[49] = 'int value50 = 0; // Ready for review'
$appLines[69] = 'int value70 = 0; // Ready for review'
Write-Fixture 'app.cpp' (($appLines -join "`n") + "`n")
Write-Fixture 'Юникод файл.txt' "Привет, мир!`nUnicode paths and content.`n"
Write-Fixture 'empty.txt' ''
[IO.File]::WriteAllBytes((Join-Path $fixtureRoot 'binary.dat'),[byte[]](0,1,2,255))
Write-Fixture 'no-newline.txt' 'No trailing newline'
Write-Fixture 'long-line.txt' ('Long line: ' + ('0123456789' * 1000) + "`n")
if($Large) {Write-Fixture 'large.txt' ((1..20000 | ForEach-Object {"Line $_"}) -join "`n")}
Git @('mv','old name.txt','renamed file.txt')
Git @('rm','deleted.txt')
Git @('add','.')
Write-Fixture 'notes.txt' "A tracked working-tree file.`nUnstaged change.`n"
Write-Fixture 'untracked.txt' "Appears in diff after git add.`n"
Write-Output $fixtureRoot
