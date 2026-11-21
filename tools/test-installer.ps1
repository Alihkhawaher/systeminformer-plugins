# Checks a built installer without changing anything: loads the exe as an assembly (no elevation, no window shown),
# lists the embedded plugins with the status it detects, and runs its settings / exclusivity logic on test data.
#   powershell -STA -File tools\test-installer.ps1 SystemInformerPlugins-Setup.exe
param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
# A crash in a window event then throws here instead of opening the .NET error dialog.
[Windows.Forms.Application]::SetUnhandledExceptionMode([Windows.Forms.UnhandledExceptionMode]::ThrowException)
$asm = [Reflection.Assembly]::LoadFile((Resolve-Path $Exe).Path)
$t = $asm.GetType('InstallerForm')
$bf = [Reflection.BindingFlags]'NonPublic,Static,Instance,Public'
$fail = 0
function Check($what, $ok) { if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:fail++ } }

$f = [Activator]::CreateInstance($t)
"title:   $($f.Text)"
"version: $($t.GetField('versionLabel', $bf).GetValue($f).Text)"
$lv = $t.GetField('list', $bf).GetValue($f)
foreach ($i in $lv.Items) { '  {0,-13} {1,-14} ticked={2}' -f $i.SubItems[0].Text, $i.SubItems[1].Text, $i.Checked }
Check 'four plugins embedded' ($lv.Items.Count -eq 4)

$set = $t.GetMethod('SetSetting', $bf)
$r = $set.Invoke($null, @("{`n`"A`":`"1`",`n`"EnableDefaultSafePlugins`":`"1`"`n}", 'EnableDefaultSafePlugins', '0'))
Check 'SetSetting replaces an existing value' ($r -eq "{`n`"A`":`"1`",`n`"EnableDefaultSafePlugins`":`"0`"`n}")
$r = $set.Invoke($null, @("{`n`"A`":`"1`"`n}", 'ExtendedTools.EnableGpuMonitor', '0'))
Check 'SetSetting adds a missing value' ($r -eq "{`n`"ExtendedTools.EnableGpuMonitor`":`"0`",`n`"A`":`"1`"`n}")

# OtherUnsignedPlugins on a fake folder: a signed DLL (with .sig) and an unsigned one.
$dir = [string](Join-Path (Split-Path $PSScriptRoot) 'obj\installer-test\plugins')
Remove-Item (Split-Path $dir) -Recurse -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir | Out-Null
'x' | Set-Content "$dir\Stock.dll"; 'x' | Set-Content "$dir\Stock.sig"; 'x' | Set-Content "$dir\GpuMonitor.dll"
$entry = $asm.GetType('PluginEntry')
$listType = [Collections.Generic.List`1].MakeGenericType($entry)
$none = [Activator]::CreateInstance($listType)
$gpu = [Activator]::CreateInstance($listType); $e = [Activator]::CreateInstance($entry); $entry.GetField('Name').SetValue($e, 'GpuMonitor'); $gpu.Add($e)
$o = $t.GetMethod('OtherUnsignedPlugins', $bf)
Check 'unsigned plugin left -> keep third-party plugins on' ($o.Invoke($null, [object[]]@($dir, $none.psobject.BaseObject)))
Check 'only signed plugins left -> back to default' (-not $o.Invoke($null, [object[]]@($dir, $gpu.psobject.BaseObject)))
Remove-Item (Split-Path $dir) -Recurse

# Creating the list's window must not throw (it raises ItemChecked on half-built rows). Then ticking NvTccGpu
# unticks GpuMonitor and back (ItemChecked only fires once the ListView has a handle).
$null = $f.Handle; $null = $lv.Handle
$t.GetField('ready', $bf).SetValue($f, $true)   # normally set by OnShown
$byName = @{}; foreach ($i in $lv.Items) { $byName[$i.SubItems[0].Text] = $i }
$byName['NvTccGpu'].Checked = $true
Check 'NvTccGpu unticks GpuMonitor' (-not $byName['GpuMonitor'].Checked)
$byName['GpuMonitor'].Checked = $true
Check 'GpuMonitor unticks NvTccGpu' (-not $byName['NvTccGpu'].Checked)
$f.Dispose()

# Show a fresh window for a moment, as a real start does (list window created, OnShown runs).
$f = [Activator]::CreateInstance($t)
try { $f.Show(); [Windows.Forms.Application]::DoEvents(); Check 'window opens without error' ($t.GetField('ready', $bf).GetValue($f)) }
catch { Check "window opens without error ($($_.Exception.InnerException.Message))" $false }
finally { $f.Close(); $f.Dispose() }
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
