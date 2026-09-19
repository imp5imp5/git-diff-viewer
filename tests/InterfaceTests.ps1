param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$Artifacts)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$fixture = & (Join-Path $project 'tools/create_test_repo.ps1') -Large
$root = Join-Path $Artifacts ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$client = Join-Path $project 'tools/app_command.ps1'
function Invoke-App([string]$Verb,[string[]]$Values=@()) { & $client -Directory $root -Command $Verb -Arguments $Values }
function Check($Condition,[string]$Message) {if(-not $Condition) {throw $Message}}
function Read-SharedJson([string]$Path) {
    try {
        $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try {
            $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
            try {return $reader.ReadToEnd() | ConvertFrom-Json} finally {$reader.Dispose()}
        } finally {$stream.Dispose()}
    } catch {
        if(-not ($_.Exception.GetBaseException() -is [IO.IOException])) {throw}
        return $null
    }
}
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
    $panels=Invoke-App 'layout' @('panels')
    Check ((@($panels.controls | Where-Object {$_.id -eq 'layout'})[0]).text -eq 'Wide Diff') 'Layout button is named Wide Diff'
    Check ($panels.layout -eq 'panels' -and -not $panels.loading) 'Panel layout switches without Git reload'
    $commitPane=$panels.controls | Where-Object {$_.id -eq 'explorer-commits'}
    $messagePane=$panels.controls | Where-Object {$_.id -eq 'explorer-message'}
    $filePane=$panels.controls | Where-Object {$_.id -eq 'explorer-files'}
    $diffPane=$panels.controls | Where-Object {$_.id -eq 'diff'}
    Check ($commitPane.visible -and $messagePane.visible -and $filePane.visible -and
        $commitPane.x -lt $filePane.x -and $filePane.x -lt $messagePane.x -and
        $diffPane.y -ge $filePane.y + $filePane.height -and $diffPane.x -eq $commitPane.x -and
        $diffPane.width -ge $filePane.x + $filePane.width - $commitPane.x) 'Three upper panels and full-width diff'
    $commitScroll=$panels.controls | Where-Object {$_.id -eq 'explorer-commits-scroll'}
    $fileScroll=$panels.controls | Where-Object {$_.id -eq 'explorer-files-scroll'}
    $messageScroll=$panels.controls | Where-Object {$_.id -eq 'explorer-message-scroll'}
    Check ($commitScroll.visible -and $fileScroll.visible -and $messageScroll.visible -and
        $commitScroll.x -gt $commitPane.x -and $fileScroll.x -gt $filePane.x -and
        $messageScroll.x -gt $messagePane.x) 'Upper panels use their own scrollbars'
    $x=$panels.explorerSplitters[0].x + 24
    $panels=Invoke-App 'explorer-splitter' @('0',"$x")
    Check ($panels.explorerSplitters[0].x -eq $x) 'Commit divider moves'
    $x=$panels.explorerSplitters[1].x + 20
    $panels=Invoke-App 'explorer-splitter' @('1',"$x")
    Check ($panels.explorerSplitters[1].x -eq $x) 'Files divider moves'
    $y=$panels.explorerSplitters[2].y + 20
    $panels=Invoke-App 'explorer-splitter' @('2',"$y")
    Check ($panels.explorerSplitters[2].y -eq $y) 'Diff divider moves'
    foreach($dragIndex in 0,1,2) {
        $divider=$panels.explorerSplitters[$dragIndex]
        $destination=if($dragIndex -eq 2) {$divider.y+20} else {$divider.x+20}
        Invoke-App 'explorer-drag' @('start',"$dragIndex") | Out-Null
        $moved=Invoke-App 'explorer-drag' @('move',"$destination")
        $paintAfterMove=$moved.explorerResizeRepaints
        Start-Sleep -Milliseconds 250
        $settled=Invoke-App 'state'
        Check ($settled.explorerResizeRepaints -gt $paintAfterMove) 'Upper panels are redrawn 100 ms after splitter movement stops'
        Start-Sleep -Milliseconds 250
        $idle=Invoke-App 'state'
        Check ($idle.explorerResizeRepaints -eq $settled.explorerResizeRepaints) 'Upper panels do not repaint continuously while drag is still held'
        $panels=Invoke-App 'explorer-drag' @('end',"$destination")
        Check ($panels.explorerResizeRepaints -gt $idle.explorerResizeRepaints) 'Upper panels are redrawn when splitter drag is released'
    }
    $panels=Invoke-App 'source' @('history')
    $panels=Wait-Idle
    Check ($panels.explorerGroups[0].label -eq 'Unstaged' -and
        $panels.explorerGroups[1].label -eq 'Staged' -and
        $panels.explorerGroups[2].label -eq 'Ready to push') 'History starts with worktree and outgoing sections'
    $historyIndex=-1
    for($i=0;$i -lt $panels.explorerGroups.Count;$i++) {
        if($panels.explorerGroups[$i].label -eq 'History' -and $panels.explorerGroups[$i].kind -eq 'section') {$historyIndex=$i;break}
    }
    Check ($historyIndex -gt 2 -and $panels.explorerGroups[$historyIndex-1].key.StartsWith('outgoing') -and
        $panels.explorerGroups[$historyIndex+1].key.StartsWith('history')) 'History divider separates outgoing and older commits'
    $historySection=Invoke-App 'select-explorer-group' @([string]$historyIndex)
    Check ($historySection.explorerGroupSelection -eq $historyIndex -and $historySection.explorerFiles.Count -eq 0 -and
        $historySection.explorerMessageText.Contains('commits loaded') -and
        $historySection.status.Contains('commits loaded')) 'History divider shows the loaded commit count'
    $panels=Invoke-App 'explorer-wheel' @('commits','-120')
    Check ($panels.explorerGroupTop -gt 0) 'Commit wheel scrolls the visible list'
    $panels=Invoke-App 'explorer-wheel' @('commits','120')
    Check ($panels.explorerGroupTop -eq 0) 'Commit wheel scrolls back'
    $panels=Invoke-App 'explorer-scrollbar' @('0','bottom')
    Check ($panels.explorerGroupTop -gt 0) 'Dragging custom commit scrollbar moves the list'
    $panels=Invoke-App 'explorer-scrollbar' @('0','top')
    Check ($panels.explorerGroupTop -eq 0) 'Custom commit scrollbar returns to the top'
    $originalTopSplit=$panels.explorerSplitters[2].y
    $panels=Invoke-App 'explorer-splitter' @('2','220')
    $panels=Invoke-App 'select-explorer-group' @('4')
    $initialCommitThumb=$panels.explorerCommitThumbTop
    1..6 | ForEach-Object {$panels=Invoke-App 'navigate-file' @('1')}
    Check ($panels.explorerGroupTop -gt 0 -and
        $panels.explorerCommitThumbTop -gt $initialCommitThumb) 'Ctrl+Down moves the commit scrollbar as rows leave view'
    $downCommitThumb=$panels.explorerCommitThumbTop
    1..6 | ForEach-Object {$panels=Invoke-App 'navigate-file' @('-1')}
    Check ($panels.explorerCommitThumbTop -lt $downCommitThumb) 'Ctrl+Up moves the commit scrollbar back'
    $panels=Invoke-App 'explorer-splitter' @('2',"$originalTopSplit")
    $panels=Invoke-App 'select-explorer-group' @('2')
    Check ($panels.explorerFiles.Count -gt 0 -and
        $panels.explorerMessageText.Contains('files changed')) 'Ready to push section shows aggregate files and totals'
    $panels=Invoke-App 'select-explorer-group' @('1')
    Check ($panels.explorerFiles.Count -eq 9 -and $panels.selectedFile -eq $panels.explorerFiles[0].path -and
        $panels.explorerMessageText.Contains('9 files changed')) 'Section selection fills files, summary, and diff'
    Check ($panels.status -match 'files changed\s+-\d+\s+\+\d+') 'Status shows removed before added'
    $panels=Invoke-App 'explorer-hover-file' @('0')
    Check ($panels.explorerTooltip.Contains($panels.explorerFiles[0].path) -and
        $panels.explorerTooltip -match '-\d+ \+\d+') 'File hint includes full path and totals'
    $panels=Invoke-App 'explorer-message-select-all'
    Check ($panels.explorerMessageSelectionEnd -eq $panels.explorerMessageText.Length) 'Ctrl+A selects the full commit message'
    $panels=Invoke-App 'select-explorer-file' @('0')
    $panels=Invoke-App 'scroll' @('2')
    $rememberedTop=$panels.topRow
    Invoke-App 'select-explorer-group' @('2') | Out-Null
    $panels=Invoke-App 'select-explorer-group' @('1')
    Check ($panels.explorerFileSelection -eq 0 -and $panels.topRow -eq $rememberedTop) 'Section restores its file and diff scroll'
    $panels=Invoke-App 'select-explorer-file' @('8')
    Invoke-App 'select-explorer-group' @('2') | Out-Null
    $panels=Invoke-App 'select-explorer-group' @('1')
    Check ($panels.explorerFileSelection -eq 8) 'Section remembers its selected file'
    $panels=Invoke-App 'select-explorer-file' @('0')
    $originalDivider=$panels.explorerSplitters[2].y
    $panels=Invoke-App 'explorer-splitter' @('2','220')
    $panels=Invoke-App 'explorer-scrollbar' @('1','bottom')
    Check ($panels.explorerFileTop -gt 0) 'Dragging custom file scrollbar moves the list'
    $panels=Invoke-App 'explorer-scrollbar' @('1','top')
    Check ($panels.explorerFileTop -eq 0) 'Custom file scrollbar returns to the top'
    $wheelSelection=$panels.selectedListKey
    $panels=Invoke-App 'explorer-wheel' @('files','-120')
    Check ($panels.explorerFileTop -gt 0 -and $panels.selectedListKey -eq $wheelSelection) 'File wheel scrolls without changing selection'
    $panels=Invoke-App 'explorer-wheel' @('files','120')
    Check ($panels.explorerFileTop -eq 0) 'File wheel scrolls back'
    $initialFileThumb=$panels.explorerFileThumbTop
    1..6 | ForEach-Object {$panels=Invoke-App 'navigate-file' @('1')}
    Check ($panels.explorerFileTop -gt 0 -and
        $panels.explorerFileThumbTop -gt $initialFileThumb) 'Ctrl+Down moves the file scrollbar as rows leave view'
    $downFileThumb=$panels.explorerFileThumbTop
    1..6 | ForEach-Object {$panels=Invoke-App 'navigate-file' @('-1')}
    Check ($panels.explorerFileThumbTop -lt $downFileThumb) 'Ctrl+Up moves the file scrollbar back'
    $panels=Invoke-App 'select-explorer-file' @('0')
    $panels=Invoke-App 'explorer-scrollbar' @('1','top')
    $panels=Invoke-App 'explorer-splitter' @('2',"$originalDivider")
    $panels=Invoke-App 'navigate-file' @('1')
    Check ($panels.explorerGroupSelection -eq 1 -and $panels.explorerFileSelection -eq 1 -and
        $panels.selectedFile -eq $panels.explorerFiles[1].path) 'Ctrl+Down highlights the next visible file'
    $panels=Invoke-App 'navigate-file' @('-1')
    Check ($panels.explorerGroupSelection -eq 1 -and $panels.explorerFileSelection -eq 0) 'Ctrl+Up highlights the previous visible file'
    $panels=Invoke-App 'select-explorer-file' @('8')
    $panels=Invoke-App 'navigate-file' @('1')
    Check ($panels.explorerGroupSelection -eq 2 -and $panels.explorerFileSelection -eq 0 -and
        $panels.selectedFile -eq $panels.explorerFiles[0].path) 'Ctrl+Down advances to the next visible section'
    $panels=Invoke-App 'navigate-file' @('-1')
    Check ($panels.explorerGroupSelection -eq 1 -and $panels.explorerFileSelection -eq 8) 'Ctrl+Up returns to the previous section and its last file'
    $commitIndex=-1
    for($i=0;$i -lt $panels.explorerGroups.Count;$i++) {
        if($panels.explorerGroups[$i].kind -eq 'commit') {$commitIndex=$i;break}
    }
    Check ($commitIndex -ge 0) 'History exposes commits'
    $panels=Invoke-App 'select-explorer-group' @("$commitIndex")
    Check ($panels.explorerFiles.Count -gt 0 -and $panels.explorerMessageText.Contains('Author:') -and
        $panels.explorerMessageText.Contains('Date:')) 'Commit selection fills message and files'
    Capture 'panels-history.png'
    $panels=Invoke-App 'select-explorer-file' @('0')
    Check ($panels.selectedFile -eq $panels.explorerFiles[0].path) 'File selection updates diff'
    $panels=Invoke-App 'scroll' @('1')
    $savedKey=$panels.selectedListKey
    $savedTop=$panels.topRow
    $panels=Invoke-App 'layout' @('classic')
    Check ($panels.selectedListKey -eq $savedKey -and $panels.topRow -eq $savedTop -and -not $panels.loading) 'Classic layout restores selection and position'
    $panels=Invoke-App 'layout' @('panels')
    Check ($panels.selectedListKey -eq $savedKey -and $panels.topRow -eq $savedTop) 'Panel layout restores selection and position'
    Invoke-App 'source' @('ready') | Out-Null
    $panels=Wait-Idle
    Check ($panels.explorerGroups[0].label -eq 'Ready to push' -and
        $panels.explorerFiles.Count -gt 0) 'Ready to push has a combined section in panel layout'
    Invoke-App 'source' @('commit') | Out-Null
    $panels=Wait-Idle
    Check ($panels.explorerGroups[0].label -eq 'Commit' -and
        $panels.explorerMessageText.Contains('Author:')) 'Single commit works in panel layout'
    Invoke-App 'source' @('range') | Out-Null
    Invoke-App 'base' @('main') | Out-Null
    Invoke-App 'target' @('HEAD') | Out-Null
    Invoke-App 'compare' | Out-Null
    $panels=Wait-Idle
    Check (@($panels.explorerGroups | Where-Object {$_.kind -eq 'commit'}).Count -gt 0 -and
        @($panels.explorerGroups | Where-Object {$_.kind -eq 'summary'}).Count -eq 1) 'Commit range works in panel layout'
    Invoke-App 'layout' @('classic') | Out-Null
    Invoke-App 'source' @('unstaged') | Out-Null
    $state=Wait-Idle
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
        $reply = Read-SharedJson (Join-Path $root 'response.json')
    } while(($null -eq $reply -or $reply.id -ne $retryId) -and [DateTime]::UtcNow -lt $retryDeadline)
    Check ($reply.id -eq $retryId -and $reply.ok -and $reply.state.fontSize -eq 12) 'Response retry does not repeat the command'
    Invoke-App 'zoom' @('-1') | Out-Null
    Invoke-App 'source' @('staged') | Out-Null
    $state=Wait-Idle
    Check ($state.files.Count -eq 9 -and $state.selectedFile -eq 'app.cpp') 'Initial staged files'
    $compactRows=$state.rowCount
    $fullLoading=Invoke-App 'full-file' @('on')
    Check ($fullLoading.loading -and $fullLoading.statusAnimating) 'Full-file mode lazily loads the selected file with status animation'
    $state=Wait-Idle
    Check ($state.fullFile -and $state.selectedFile -eq 'app.cpp' -and $state.rowCount -gt $compactRows -and $state.minimapMarkers -eq 3) 'Full-file mode loads omitted context and shows minimap markers'
    Check ($state.activeChangeStart -gt 0 -and $state.topRow -gt 0 -and -not $state.changeFlashing) 'First full-file visit centers the first change without flashing'
    $fullFileButton=$state.controls | Where-Object {$_.id -eq 'full-file'}
    Check ($fullFileButton.visible -and $fullFileButton.text -eq 'Full file') 'Full-file button is visible'
    Capture 'full-file.png'
    $state=Invoke-App 'scrollbar' @('hover')
    Check ($state.scrollbarHover -and $state.minimapMarkers -eq 3) 'Custom scrollbar hover keeps minimap markers'
    Capture 'full-file-scrollbar-hover.png'
    $state=Invoke-App 'scrollbar' @('leave')
    Check (-not $state.scrollbarHover) 'Custom scrollbar hover state clears'
    $firstChange=$state.activeChangeStart
    $state=Invoke-App 'navigate-change' @('next')
    $blockHeight=$state.activeChangeEnd-$state.activeChangeStart+1
    $expectedTop=$state.activeChangeStart-[Math]::Floor(($state.visibleRowCount-$blockHeight)/2)
    Check ($state.activeChangeStart -gt $firstChange -and $state.topRow -eq $expectedTop -and $state.changeFlashing) 'Next change is centered and flashes'
    Start-Sleep -Milliseconds 150
    $state=Invoke-App 'state'
    Check (-not $state.changeFlashing) 'Change flash ends after 0.1 seconds'
    $nextChange=$state.activeChangeStart
    Invoke-App 'navigate-change' @('next') | Out-Null
    $state=Invoke-App 'navigate-change' @('previous')
    Check ($state.activeChangeStart -eq $nextChange -and $state.changeFlashing) 'Previous change navigation returns to the preceding block'
    $savedAppTop=$state.topRow
    Invoke-App 'select-file' @('large.txt') | Out-Null
    $firstLarge=Wait-Idle
    Check ($firstLarge.activeChangeStart -ge 0 -and $firstLarge.topRow -eq $firstLarge.activeChangeStart) 'First full-file visit moves to the first change'
    $state=Invoke-App 'select-file' @('app.cpp')
    Check ($state.topRow -eq $savedAppTop -and $state.activeChangeStart -eq -1) 'A previously visited full file restores its scroll position'
    Invoke-App 'refresh' | Out-Null
    $state=Wait-Idle
    Check ($state.selectedFile -eq 'app.cpp' -and $state.topRow -eq $savedAppTop) 'Refresh preserves per-file scroll position'
    Invoke-App 'full-file' @('off') | Out-Null
    $state=Wait-Idle
    Check (-not $state.fullFile -and $state.rowCount -eq $compactRows -and $state.minimapMarkers -eq 0) 'Full-file mode can be disabled'
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
    Invoke-App 'scroll' @('0') | Out-Null
    $pressed=Invoke-App 'scrollbar' @('press-middle')
    Check ($pressed.scrollbarDragging -and $pressed.topRow -gt 9000 -and $pressed.topRow -lt 11000) 'Scrollbar track press centers the thumb and begins dragging'
    $released=Invoke-App 'scrollbar' @('release')
    Check (-not $released.scrollbarDragging) 'Scrollbar dragging ends on mouse release'
    Invoke-App 'scroll' @('0') | Out-Null
    $dragged=Invoke-App 'scrollbar' @('drag-bottom')
    Check ($dragged.topRow -gt 19900) 'Custom scrollbar thumb can be dragged to the bottom'
    Invoke-App 'scroll' @('0') | Out-Null
    $largeChange=Invoke-App 'navigate-change' @('next')
    Check ($largeChange.topRow -eq $largeChange.activeChangeStart -and $largeChange.changeFlashing) 'A change block taller than the viewport keeps its first line visible'
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
    $state=Invoke-App 'splitter' @('360')
    $filesControl=$state.controls | Where-Object {$_.id -eq 'files'}
    $diffControl=$state.controls | Where-Object {$_.id -eq 'diff'}
    Check ($filesControl.x + $filesControl.width -eq 360 -and $diffControl.x -gt 360) 'Dragging splitter resizes file and diff panes'
    Capture 'resized.png'
    Invoke-App 'source' @('unstaged') | Out-Null
    $state=Wait-Idle
    Check ($state.files.Count -eq 1 -and $state.files[0].path -eq 'notes.txt') 'Unstaged source'
    Invoke-App 'source' @('ready') | Out-Null
    $state=Wait-Idle
    Check ($state.commits.Count -eq 2 -and $state.files.Count -eq 2 -and $state.fileList.Count -eq 8) 'Ready to push groups files by commit'
    Check ($state.fileList[0].kind -eq 'commit' -and $state.fileList[0].label.EndsWith('Add first feature') -and
           $state.fileList[1].kind -eq 'file' -and $state.fileList[1].path -eq 'first.txt' -and
           $state.fileList[2].kind -eq 'commit' -and $state.fileList[2].label.EndsWith('Add second feature') -and
           $state.fileList[3].kind -eq 'file' -and $state.fileList[3].path -eq 'second.txt' -and
           $state.fileList[4].kind -eq 'spacer' -and $state.fileList[5].kind -eq 'summary') 'Ready to push list order'
    $commitControl=$state.controls | Where-Object {$_.id -eq 'commits'}
    Check (-not $commitControl.visible) 'Ready to push uses the changed-files list instead of the commit dropdown'
    $readyMessage=Invoke-App 'select-list-item' @('2')
    Check ($readyMessage.plainText -and @($readyMessage.visibleRows | Where-Object {$_.meta -eq 'Add second feature'}).Count -eq 1) 'Ready commit heading opens its message'
    Check ($readyMessage.status -like '1 files changed*-*+1*') 'Ready commit heading shows its statistics'
    $readyFile=Invoke-App 'select-list-item' @('3')
    Check ($readyFile.selectedFile -eq 'second.txt' -and -not $readyFile.plainText) 'Ready commit file opens its commit diff'
    Check ($readyFile.status -like '1 files changed*-*+1*') 'Ready commit file keeps its commit statistics'
    $readySummaryHeading=Invoke-App 'select-list-item' @('5')
    Check (@($readySummaryHeading.visibleRows | Where-Object {$_.meta -match '^[0-9a-f]{8}  Add first feature$'}).Count -eq 1 -and
           @($readySummaryHeading.visibleRows | Where-Object {$_.meta -match '^[0-9a-f]{8}  Add second feature$'}).Count -eq 1) 'Ready summary lists commit hashes and subjects'
    $readySummary=Invoke-App 'select-file' @('second.txt')
    Check ($readySummary.selectedListKey -eq "summary`nsecond.txt" -and $readySummary.status -like '2 files changed*-*+2*') 'Ready summary file uses the combined diff and statistics'
    Capture 'ready.png'
    Invoke-App 'base' @('HEAD~1') | Out-Null
    Invoke-App 'compare' | Out-Null
    $state=Wait-Idle
    Check ($state.fileList.Count -eq 2 -and $state.fileList[0].kind -eq 'commit' -and
           $state.fileList[1].kind -eq 'file' -and $state.fileList[1].path -eq 'second.txt') 'One ready commit omits the redundant summary'
    Invoke-App 'source' @('commit') | Out-Null
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
    Check ($first.selectedFile -eq '<<Commit Message>>' -and $first.plainText) 'List navigation includes the commit message entry'
    Invoke-App 'source' @('commit') | Out-Null
    $state=Wait-Idle
    Check ($state.files[0].path -eq '<<Commit Message>>') 'Single commit mode also includes message'
    Invoke-App 'base' @('main') | Out-Null
    Invoke-App 'target' @('HEAD') | Out-Null
    Invoke-App 'source' @('range') | Out-Null
    $state=Wait-Idle
    Check ($state.files.Count -eq 2 -and $state.fileList.Count -eq 8) 'Commit range includes grouped and summary files'
    Check ($state.fileList[0].kind -eq 'commit' -and $state.fileList[0].label.EndsWith('Add first feature') -and
           $state.fileList[1].kind -eq 'file' -and $state.fileList[1].path -eq 'first.txt' -and
           $state.fileList[2].kind -eq 'commit' -and $state.fileList[2].label.EndsWith('Add second feature') -and
           $state.fileList[3].kind -eq 'file' -and $state.fileList[3].path -eq 'second.txt' -and
           $state.fileList[4].kind -eq 'spacer' -and $state.fileList[5].kind -eq 'summary') 'Commit range list order'
    $message=Invoke-App 'select-list-item' @('0')
    Check ($message.plainText -and @($message.visibleRows | Where-Object {$_.meta -eq 'Add first feature'}).Count -eq 1) 'Range commit heading opens its message'
    Check ($message.status -like '1 files changed*-*+1*') 'Commit heading shows statistics for that commit'
    $commitFile=Invoke-App 'select-list-item' @('1')
    Check (-not $commitFile.plainText -and $commitFile.selectedListKey.StartsWith("commit`n") -and $commitFile.selectedFile -eq 'first.txt') 'Range commit file opens its commit diff'
    Check ($commitFile.status -like '1 files changed*-*+1*') 'Commit file keeps statistics for its commit'
    $summaryFile=Invoke-App 'select-file' @('second.txt')
    Check ($summaryFile.selectedListKey -eq "summary`nsecond.txt") 'File selection prefers the combined range summary'
    Check ($summaryFile.status -like '2 files changed*-*+2*') 'Summary file shows statistics for the complete range'
    $summaryFirst=Invoke-App 'navigate-file' @('-1')
    Check ($summaryFirst.selectedListKey -eq "summary`nfirst.txt") 'Ctrl list navigation selects summary files'
    $summary=Invoke-App 'navigate-file' @('-1')
    Check ($summary.selectedListKey -eq 'summary' -and $summary.selectedFile -eq '<<Summary>>') 'Ctrl list navigation selects the summary heading'
    Check ($summary.status -like '2 files changed*-*+2*') 'Summary heading shows statistics for the complete range'
    Check (@($summary.visibleRows | Where-Object {$_.meta -match '^[0-9a-f]{8}  Add first feature$'}).Count -eq 1 -and
           @($summary.visibleRows | Where-Object {$_.meta -match '^[0-9a-f]{8}  Add second feature$'}).Count -eq 1) 'Range summary lists commit hashes and subjects'
    $commitFile=Invoke-App 'navigate-file' @('-1')
    Check ($commitFile.selectedListKey.StartsWith("commit`n") -and $commitFile.selectedFile -eq 'second.txt') 'Ctrl list navigation skips the empty row'
    $commitHeading=Invoke-App 'navigate-file' @('-1')
    Check ($commitHeading.selectedFile -eq '<<Commit Message>>' -and $commitHeading.plainText) 'Ctrl list navigation selects commit headings'
    Invoke-App 'select-list-item' @('3') | Out-Null
    $summary=Invoke-App 'list-key' @('down')
    Check ($summary.selectedListKey -eq 'summary') 'Down skips the empty row and selects the summary heading'
    $commitFile=Invoke-App 'list-key' @('up')
    Check ($commitFile.selectedFile -eq 'second.txt' -and $commitFile.selectedListKey.StartsWith("commit`n")) 'Up skips the empty row'
    $commitHeading=Invoke-App 'list-key' @('up')
    Check ($commitHeading.selectedFile -eq '<<Commit Message>>' -and $commitHeading.plainText) 'Up selects commit headings'
    Capture 'commit-range.png'
    Invoke-App 'base' @('HEAD~1') | Out-Null
    Invoke-App 'compare' | Out-Null
    $state=Wait-Idle
    Check ($state.fileList.Count -eq 2 -and
           $state.fileList[0].kind -eq 'commit' -and $state.fileList[0].label.EndsWith('Add second feature') -and
           $state.fileList[1].kind -eq 'file' -and $state.fileList[1].path -eq 'second.txt') 'Single-commit range omits the redundant summary'
    Invoke-App 'source' @('history') | Out-Null
    $state=Wait-Idle
    Check ($state.source -eq 6) 'History is the final source without changing the default source value'
    Check ($state.fileList[0].kind -eq 'section' -and $state.fileList[0].label -eq 'Unstaged' -and
           $state.fileList[1].kind -eq 'file' -and $state.fileList[1].path -eq 'notes.txt' -and
           $state.fileList[2].kind -eq 'spacer' -and
           $state.fileList[3].kind -eq 'section' -and $state.fileList[3].label -eq 'Staged' -and
           $state.fileList[13].kind -eq 'spacer' -and
           $state.fileList[14].kind -eq 'section' -and $state.fileList[14].label -eq 'Ready to push' -and
           $state.fileList[19].kind -eq 'spacer' -and
           $state.fileList[20].kind -eq 'section' -and $state.fileList[20].label -eq 'History') 'History section order and three empty separators'
    Check (@($state.fileList | Where-Object {$_.kind -eq 'spacer'}).Count -eq 3) 'History has exactly three spacer rows'
    $historyCommits=@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")})
    Check ($historyCommits.Count -eq 10 -and $historyCommits[0].label.EndsWith('History commit 48') -and
           $historyCommits[1].label.EndsWith('History commit 47') -and
           @($historyCommits | Where-Object {$_.label.EndsWith('Add second feature') -or $_.label.EndsWith('Add first feature')}).Count -eq 0) 'History initially contains 10 newest-first non-outgoing commits'
    Check (@($state.fileList | Where-Object {$_.kind -eq 'load-more'}).Count -eq 1) 'History initially exposes Load more'
    $outgoingFirst=@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("outgoing`n")})[0]
    Check ($outgoingFirst.label.EndsWith('Add second feature')) 'Ready to push keeps outgoing commits excluded from History'
    $unstagedSummary=Invoke-App 'select-list-item' @('0')
    Check ($unstagedSummary.plainText -and $unstagedSummary.message -eq '' -and
           @($unstagedSummary.visibleRows | Where-Object {$_.meta -eq 'Unstaged'}).Count -eq 1) 'Unstaged heading opens its summary'
    $readySummary=Invoke-App 'select-list-item' @('14')
    Check ($readySummary.plainText -and
           @($readySummary.visibleRows | Where-Object {$_.meta -eq 'Add second feature'}).Count -eq 1 -and
           @($readySummary.visibleRows | Where-Object {$_.meta -eq 'Add first feature'}).Count -eq 1 -and
           @($readySummary.visibleRows | Where-Object {$_.meta -eq ('_' * 80)}).Count -eq 1) 'History Ready to push heading separates full commit messages'
    $historySummary=Invoke-App 'select-list-item' @('20')
    Check ($historySummary.plainText -and $historySummary.rowCount -gt 50 -and
           @($historySummary.visibleRows | Where-Object {$_.meta -eq 'History commit 48'}).Count -eq 1 -and
           @($historySummary.visibleRows | Where-Object {$_.meta -eq 'Add second feature'}).Count -eq 0) 'History heading shows loaded non-outgoing commit messages'
    $historyMessage=Invoke-App 'select-list-item' @('21')
    Check ($historyMessage.plainText -and @($historyMessage.visibleRows | Where-Object {$_.meta -eq 'History commit 48'}).Count -eq 1) 'History commit heading opens its message'
    $historyFile=Invoke-App 'select-list-item' @('22')
    Check (-not $historyFile.plainText -and $historyFile.selectedFile -eq 'history.txt' -and
           $historyFile.selectedListKey.StartsWith("history`n")) 'History commit file opens that commit diff'
    $stagedAppIndex=-1
    for($i=0;$i -lt $state.fileList.Count;$i++) {if($state.fileList[$i].key -eq "history-staged`napp.cpp") {$stagedAppIndex=$i;break}}
    Check ($stagedAppIndex -ge 0) 'History staged file is present'
    $beforeMore=Invoke-App 'select-list-item' @([string]$stagedAppIndex)
    $panelPage=Invoke-App 'layout' @('panels')
    $loadMoreGroup=-1
    for($i=0;$i -lt $panelPage.explorerGroups.Count;$i++) {
        if($panelPage.explorerGroups[$i].kind -eq 'load-more') {$loadMoreGroup=$i;break}
    }
    Check ($loadMoreGroup -ge 0) 'Panel History puts Load more at the end'
    Invoke-App 'scroll' @('2') | Out-Null
    $loadingMore=Invoke-App 'select-explorer-group' @([string]$loadMoreGroup)
    Check ($loadingMore.loading -and $loadingMore.statusAnimating -and
           @($loadingMore.fileList | Where-Object {$_.kind -eq 'load-more' -and $_.label -eq 'Loading...'}).Count -eq 1) 'Automation activates Load more with status animation without clearing the list'
    $loadMoreTop=$loadingMore.explorerGroupTop
    $state=Wait-Idle
    Check (@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")}).Count -eq 20 -and
           @($state.fileList | Where-Object {$_.kind -eq 'load-more'}).Count -eq 1 -and -not $state.statusAnimating) 'Load more appends ten commits and stops status animation'
    Check ($state.selectedListKey -eq $beforeMore.selectedListKey -and $state.selectedFile -eq 'app.cpp' -and $state.topRow -eq 2) 'Load more preserves selection and diff position'
    Check (@($state.explorerGroups | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith('history')}).Count -eq 20) 'Panel History appends ten commits'
    Check ($state.explorerGroupTop -eq $loadMoreTop) 'Load more keeps the commit list scroll position'
    Invoke-App 'layout' @('classic') | Out-Null
    Invoke-App 'refresh' | Out-Null
    $state=Wait-Idle
    Check (@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")}).Count -eq 20) 'History Refresh preserves the expanded limit'
    Invoke-App 'source' @('staged') | Out-Null
    $state=Wait-Idle
    Invoke-App 'source' @('history') | Out-Null
    $state=Wait-Idle
    Check (@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")}).Count -eq 20) 'Returning to History preserves the expanded limit'
    $loadMoreIndex=-1
    for($i=0;$i -lt $state.fileList.Count;$i++) {if($state.fileList[$i].kind -eq 'load-more') {$loadMoreIndex=$i;break}}
    Check ($loadMoreIndex -gt 0) 'Final History page is available'
    Invoke-App 'select-list-item' @([string]($loadMoreIndex-1)) | Out-Null
    Invoke-App 'list-key' @('down') | Out-Null
    Invoke-App 'list-key' @('space') | Out-Null
    $state=Wait-Idle
    Check (@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")}).Count -eq 30 -and
           @($state.fileList | Where-Object {$_.kind -eq 'load-more'}).Count -eq 1) 'Space activates Load more'
    Invoke-App 'load-more-input' @('enter') | Out-Null
    $state=Wait-Idle
    Check (@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")}).Count -eq 40 -and
           @($state.fileList | Where-Object {$_.kind -eq 'load-more'}).Count -eq 1) 'Enter activates Load more'
    Invoke-App 'load-more-input' @('mouse') | Out-Null
    $state=Wait-Idle
    Check (@($state.fileList | Where-Object {$_.kind -eq 'commit' -and $_.key.StartsWith("history`n")}).Count -eq 49 -and
           @($state.fileList | Where-Object {$_.kind -eq 'load-more'}).Count -eq 0) 'Mouse activates the final page and removes Load more at history end'
    Capture 'history.png'
    $emptyRepository=Join-Path $root 'empty-repository'
    New-Item -ItemType Directory -Path $emptyRepository | Out-Null
    & git.exe -C $emptyRepository init -b main | Out-Null
    Check ($LASTEXITCODE -eq 0) 'Create empty History fixture'
    Invoke-App 'open' @($emptyRepository) | Out-Null
    $state=Wait-Idle
    Check ($state.source -eq 6 -and @($state.fileList | Where-Object {$_.kind -eq 'notice'}).Count -eq 3 -and
           @($state.fileList | Where-Object {$_.kind -eq 'spacer'}).Count -eq 3 -and
           @($state.fileList | Where-Object {$_.kind -eq 'load-more'}).Count -eq 0) 'Empty repository History shows all section explanations'
    Check (@($state.fileList | Where-Object {$_.label -eq 'No upstream configured.'}).Count -eq 1) 'History explains a missing upstream'
    $outgoingSpacerIndex=-1
    for($i=0;$i -lt $state.fileList.Count;$i++) {if($state.fileList[$i].key -eq 'history-spacer-outgoing') {$outgoingSpacerIndex=$i;break}}
    $lastSpacer=Invoke-App 'select-list-item' @([string]$outgoingSpacerIndex)
    Check ($lastSpacer.selectedListKey -eq "history-section`ncommits") 'Trailing spacer redirects safely to the History heading'
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
