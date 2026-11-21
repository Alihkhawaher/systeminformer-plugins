# Removes plugins installed by install.ps1.
#   .\uninstall.ps1                  # every plugin under plugins\
#   .\uninstall.ps1 -Plugin NvTccGpu
# When none of this repo's plugins remain, restores EnableDefaultSafePlugins=1 (System Informer's default).
# Self-elevates (one UAC prompt).
param(
    [string[]]$Plugin,
    [string]$SystemInformerDir = "$env:ProgramFiles\SystemInformer",
    [string]$SettingsPath = "$env:APPDATA\SystemInformer\settings.json"
)
$ErrorActionPreference = 'Stop'
$all = (Get-ChildItem (Join-Path $PSScriptRoot 'plugins') -Directory).Name
if (-not $Plugin) { $Plugin = $all }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-Plugin', ($Plugin -join ','), '-SystemInformerDir', "`"$SystemInformerDir`"", '-SettingsPath', "`"$SettingsPath`"")
    exit $p.ExitCode
}
$Plugin = $Plugin -split ','

$running = @(Get-Process SystemInformer -ErrorAction SilentlyContinue)
$running | Stop-Process -Force
$running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue

foreach ($name in $Plugin) {
    Remove-Item (Join-Path $SystemInformerDir "plugins\$name.dll") -ErrorAction SilentlyContinue
}

$left = @($all | Where-Object { Test-Path (Join-Path $SystemInformerDir "plugins\$_.dll") })
if (-not $left -and (Test-Path $SettingsPath)) {
    $text = [IO.File]::ReadAllText($SettingsPath)
    $new = $text -replace '"EnableDefaultSafePlugins":"0"', '"EnableDefaultSafePlugins":"1"'
    if ($new -ne $text) { [IO.File]::WriteAllText($SettingsPath, $new, (New-Object Text.UTF8Encoding $false)) }
}
Start-Process (Join-Path $SystemInformerDir 'SystemInformer.exe')
