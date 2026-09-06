param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [string]$Command = 'state',
    [string[]]$Arguments = @(),
    [int]$TimeoutSeconds = 15
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Directory).Path
$request = Join-Path $root 'request.txt'
if(Test-Path -LiteralPath $request) {throw 'A request is already pending. Use one client per automation directory.'}
$requestId = [guid]::NewGuid().ToString('N')
$fields = @($requestId,$Command) + $Arguments
foreach($field in $fields) {if($field -match "[`r`n]") {throw 'Arguments cannot contain newlines.'}}
$temporary = Join-Path $root 'request.tmp'
[IO.File]::WriteAllText($temporary,($fields -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $temporary -Destination $request
$responsePath = Join-Path $root 'response.json'
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
do {
    if(Test-Path -LiteralPath $responsePath) {
        $response = $null
        try {
            # Allow the app to atomically replace the response while we read the previous one.
            $stream = [IO.File]::Open($responsePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            try {
                $reader = [IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
                try {$response = $reader.ReadToEnd() | ConvertFrom-Json} finally {$reader.Dispose()}
            } finally {$stream.Dispose()}
        } catch {
            if(-not ($_.Exception.GetBaseException() -is [IO.IOException])) {throw}
        }
        if($null -ne $response -and $response.id -eq $requestId) {
            if(-not $response.ok) {throw $response.error}
            return $response.state
        }
    }
    Start-Sleep -Milliseconds 50
} while([DateTime]::UtcNow -lt $deadline)
throw "Timed out waiting for '$Command'. Check that the app is running with --automation-dir pointing to this directory."
