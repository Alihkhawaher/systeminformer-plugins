# Checks ProcessLog from a normal (non-elevated) shell by reading its files; System Informer must be running.
#   .\plugins\ProcessLog\test-processlog.ps1                       # runs `cmd /c exit 3`, shows its log rows
#   .\plugins\ProcessLog\test-processlog.ps1 -CmdArgs 'exit 7'      # runs `cmd /c exit 7`
#   .\plugins\ProcessLog\test-processlog.ps1 -NoRun                # only status + log health
# Prints only the test process's command line, never other rows' command lines.
param(
    [string]$CmdArgs = 'exit 3',   # what `cmd /c` runs; also how its row is found
    [switch]$NoRun,
    [string]$LogDir = "$env:APPDATA\SystemInformer"
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $LogDir 'ProcessLog.tsv'

function Read-Shared([string]$Path) {
    # System Informer keeps the log open for appending: open with ReadWrite sharing.
    if (-not (Test-Path $Path)) { return '' }
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite,Delete')
    try { (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Close() }
}

function Get-Rows([string]$Text) {
    foreach ($line in $Text -split "`n") {
        if (-not $line -or $line[0] -eq '#') { continue }
        $c = $line.TrimEnd("`r") -split "`t"
        if ($c.Count -lt 16) { continue }
        [pscustomobject]@{ Kind = $c[0]; Pid = $c[1]; Key = $c[2]; Start = [int64]$c[3]; ParentPid = $c[4]; Parent = $c[5]
            Creator = $c[7]; Name = $c[9]; Image = $c[10]; CommandLine = $c[11]; Flags = [Convert]::ToInt32($c[13], 16)
            Exit = [int64]$c[14]; ExitCode = $c[15] }
    }
}

if (-not $NoRun) {
    cmd.exe /c $CmdArgs | Out-Null
    "ran: cmd /c $CmdArgs (exit $LASTEXITCODE) from PID $PID; waiting for ETW delivery..."
    Start-Sleep -Seconds 3
}

"--- status"
Read-Shared (Join-Path $LogDir 'ProcessLog.status.txt')

"--- files"
Get-ChildItem $LogDir -Filter 'ProcessLog.tsv*' | ForEach-Object { "{0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB) }

$rows = @(Get-Rows ((Read-Shared "$log.1") + (Read-Shared $log)))
"--- log health ($($rows.Count) lines)"
$e = @($rows | Where-Object Kind -eq 'E')
"exit lines: $($e.Count); exit-not-seen: $(@($e | Where-Object { $_.Flags -band 4 }).Count); start-not-seen: $(@($e | Where-Object { $_.Flags -band 8 }).Count)"
# A process with a real exit that a later "exit not seen" line contradicts = loader/ordering bug.
$bad = $e | Group-Object Pid, Key | Where-Object { $_.Name -notmatch ', 0$' -and ($_.Group | Where-Object Exit -ne 0) -and ($_.Group | Where-Object { $_.Flags -band 4 }) }
"real exit + exit-not-seen for the same process: $(@($bad).Count)"
$bad | Select-Object -Last 5 | ForEach-Object { $s = ($_.Group | Where-Object Start -ne 0 | Select-Object -First 1).Start
    "  started {0}" -f $(if ($s) { [DateTime]::FromFileTime($s).ToString('yyyy-MM-dd HH:mm:ss') } else { '?' }) }

if (-not $NoRun) {
    "--- rows for the test process"
    $rows | Where-Object { $_.CommandLine -match [regex]::Escape($CmdArgs) -and $_.ParentPid -eq "$PID" } | Select-Object -Last 4 | ForEach-Object {
        $ms = if ($_.Exit -and $_.Start) { '{0:N1} ms' -f (($_.Exit - $_.Start) / 10000) } else { '-' }
        "{0} pid={1} parent={2}({3}) image={4} exitCode={5} duration={6} flags={7:x}" -f $_.Kind, $_.Pid, $_.Parent, $_.ParentPid, $_.Image, $_.ExitCode, $ms, $_.Flags
    }
}
