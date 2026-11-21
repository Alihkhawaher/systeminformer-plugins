# Installs bin\<Name>.dll into System Informer's plugins folder and lets it load third-party plugins.
#   .\install.ps1                  # every built plugin in bin\
#   .\install.ps1 -Plugin NvTccGpu
#   .\install.ps1 -Plugin GpuMonitor -Remove NvTccGpu -SetSetting 'ExtendedTools.EnableGpuMonitor=0'
#     -Remove      plugin DLLs to delete from System Informer in the same run
#     -SetSetting  'Name=Value' entries to change in settings.json (only existing settings)
# Self-elevates (one UAC prompt). Stops System Informer and starts it again. Undo with uninstall.ps1.
param(
    [string[]]$Plugin,
    [string[]]$Remove,
    [string[]]$SetSetting,
    [string]$SystemInformerDir = "$env:ProgramFiles\SystemInformer",
    [string]$SettingsPath = "$env:APPDATA\SystemInformer\settings.json"
)
$ErrorActionPreference = 'Stop'
if (-not $Plugin) { $Plugin = (Get-ChildItem (Join-Path $PSScriptRoot 'bin') -Filter *.dll).BaseName }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    # No -Wait: in PowerShell 5.1 it also waits for children, i.e. the System Informer we start.
    $p = Start-Process powershell -Verb RunAs -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-Plugin', ($Plugin -join ','),
        '-Remove', "`"$(@($Remove) -join ',')`"",
        '-SetSetting', "`"$(@($SetSetting) -join ';')`"",
        '-SystemInformerDir', "`"$SystemInformerDir`"", '-SettingsPath', "`"$SettingsPath`"")
    $p.WaitForExit()
    exit $p.ExitCode
}
$Plugin = $Plugin -split ','
$Remove = @($Remove -split ',' | Where-Object { $_ })
$SetSetting = @($SetSetting -split ';' | Where-Object { $_ })

$log = Join-Path $PSScriptRoot 'install.log'
"$(Get-Date -Format s) install $($Plugin -join ', ')" | Out-File $log

try {
    foreach ($name in $Plugin) {
        if (-not (Test-Path (Join-Path $PSScriptRoot "bin\$name.dll"))) { throw "missing bin\$name.dll - run build.ps1" }
    }

    # 1. Stop System Informer (HideOnClose would only hide it) so it can't overwrite settings on exit.
    $running = @(Get-Process SystemInformer -ErrorAction SilentlyContinue)
    $running | Stop-Process -Force
    $running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    "stopped $($running.Count) SystemInformer process(es)" | Out-File $log -Append

    # 2. Allow non-default plugins: EnableDefaultSafePlugins 1 -> 0 (backup kept next to settings.json).
    if (Test-Path $SettingsPath) {
        $backup = "$SettingsPath.bak-plugins"
        if (-not (Test-Path $backup)) { Copy-Item $SettingsPath $backup }
        $text = [IO.File]::ReadAllText($SettingsPath)
        $new = $text -replace '"EnableDefaultSafePlugins":"1"', '"EnableDefaultSafePlugins":"0"'
        "settings: EnableDefaultSafePlugins=0 (backup $backup)" | Out-File $log -Append
        foreach ($entry in $SetSetting) {
            $name, $value = $entry -split '=', 2
            $pattern = '"' + [regex]::Escape($name) + '":"[^"]*"'
            if ($new -match $pattern) {
                $new = [regex]::Replace($new, $pattern, ('"' + $name + '":"' + $value + '"'))
                "settings: $name=$value" | Out-File $log -Append
            } else {
                "settings: $name not found, not changed" | Out-File $log -Append
            }
        }
        if ($new -ne $text) { [IO.File]::WriteAllText($SettingsPath, $new, (New-Object Text.UTF8Encoding $false)) }
    } else {
        "settings file not found: $SettingsPath (set EnableDefaultSafePlugins to 0 in System Informer)" | Out-File $log -Append
    }

    # 3. Copy the plugins. The old DLL can stay locked for a few seconds after System Informer exits: retry.
    foreach ($name in $Plugin) {
        $target = Join-Path $SystemInformerDir "plugins\$name.dll"
        for ($try = 1; ; $try++) {
            try { Copy-Item (Join-Path $PSScriptRoot "bin\$name.dll") $target -Force; break }
            catch {
                if ($try -ge 20) {
                    $holders = Get-Process | Where-Object { try { $_.Modules.FileName -contains $target } catch { $false } }
                    throw "$name.dll still locked after 20 s (held by: $(($holders | ForEach-Object { "$($_.Name) $($_.Id)" }) -join ', '))"
                }
                Start-Sleep -Seconds 1
            }
        }
        "copied $name.dll (attempt $try)" | Out-File $log -Append
    }

    # 4. Remove plugins this install replaces.
    foreach ($name in $Remove) {
        $target = Join-Path $SystemInformerDir "plugins\$name.dll"
        if (Test-Path $target) {
            for ($try = 1; ; $try++) {
                try { Remove-Item $target -Force; break }
                catch { if ($try -ge 20) { throw "could not remove $name.dll: $_" }; Start-Sleep -Seconds 1 }
            }
            "removed $name.dll" | Out-File $log -Append
        }
    }
    $failed = $false
}
catch {
    "ERROR: $_" | Out-File $log -Append
    $failed = $true
}
finally {
    # 5. Always start System Informer again, even after an error.
    Start-Process (Join-Path $SystemInformerDir 'SystemInformer.exe')
    "started SystemInformer" | Out-File $log -Append
}
if ($failed) { exit 1 }
