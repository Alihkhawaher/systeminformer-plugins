# Builds plugins\<Name>\*.c + <Name>.rc into bin\<Name>.dll (x64 Release) against sdk\ (tools\make_sdk.py).
#   .\build.ps1                 # every plugin under plugins\
#   .\build.ps1 -Plugin NvTccGpu
#   .\build.ps1 -Probe          # only bin\memprobe.exe (console check of plugins\MemBreakdown\memcore.c)
#   .\build.ps1 -Installer -Version 1.2.0
#       every plugin, then release\SystemInformerPlugins-Setup-<Version>.exe (installer\Installer.cs, the DLLs
#       embedded; built with the .NET Framework csc.exe that ships with Windows) and
#       release\SystemInformerPlugins-<Version>.zip (the DLLs).
param([string[]]$Plugin, [switch]$Probe, [switch]$Installer, [string]$Version)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vs) { throw 'Visual Studio not found' }

if ($Probe) {
    New-Item -ItemType Directory -Force bin, obj\memprobe | Out-Null
    $step = 'cl /nologo /std:clatest /O2 /MT /W3 /utf-8 /DWIN64 /DNDEBUG /DUNICODE /D_UNICODE ' +
        '/I vendor\systeminformer\phnt\include /I plugins\MemBreakdown /Foobj\memprobe\ /Febin\memprobe.exe ' +
        'tools\memprobe\memprobe.c plugins\MemBreakdown\memcore.c plugins\MemBreakdown\filenames.c ntdll.lib kernel32.lib advapi32.lib'
    cmd /c ("call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && " + $step)
    if ($LASTEXITCODE) { throw "memprobe build failed ($LASTEXITCODE)" }
    'Built bin\memprobe.exe'
    return
}

if (-not (Test-Path sdk\lib\SystemInformer.lib)) { throw 'sdk\ missing - run: python tools\make_sdk.py' }
if ($Installer) {
    if (-not $Version) { throw '-Installer needs -Version (e.g. 1.2.0)' }
    $Plugin = $null   # a release always has every plugin
}
if (-not $Plugin) { $Plugin = (Get-ChildItem plugins -Directory).Name }

$defs = '/DWIN64 /DNDEBUG /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE /DENABLE_RESTRICTED /DCONST_VTABLE /DSI_NO_WPP'
$libs = 'SystemInformer.lib ntdll.lib user32.lib gdi32.lib kernel32.lib comctl32.lib shell32.lib advapi32.lib pdh.lib setupapi.lib cfgmgr32.lib'

foreach ($name in $Plugin) {
    $dir = "plugins\$name"
    if (-not (Test-Path $dir)) { throw "no plugin folder $dir" }
    $obj = "obj\$name"
    New-Item -ItemType Directory -Force bin, $obj | Out-Null
    $sources = (Get-ChildItem $dir -Filter *.c).FullName | ForEach-Object { "`"$_`"" }
    $inc = "/I sdk\include /I vendor\systeminformer\phnt\include /I common /I `"$dir`""   # common\ = headers shared by plugins (trayicon.h)

    $steps = @()
    $rc = Join-Path $dir "$name.rc"
    $res = ''
    if (Test-Path $rc) {
        $steps += "rc /nologo /i sdk\include /i `"$dir`" /fo $obj\$name.res `"$rc`""
        $res = "$obj\$name.res"
    }
    $steps += "cl /nologo /c /std:clatest /O2 /MT /W3 /Gz /GS /guard:cf /utf-8 $defs $inc /Fo$obj\ $($sources -join ' ')"
    $steps += "link /nologo /DLL /OUT:bin\$name.dll /MACHINE:X64 /SUBSYSTEM:WINDOWS /GUARD:CF /DYNAMICBASE /NXCOMPAT /LIBPATH:sdk\lib $obj\*.obj $res $libs"

    cmd /c ("call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && " + ($steps -join ' && '))
    if ($LASTEXITCODE) { throw "$name build failed ($LASTEXITCODE)" }
    "Built bin\$name.dll"
}

if ($Installer) {
    # System Informer version the SDK was made from (vendor\systeminformer is a clone of tag v<version>).
    $siVersion = (git -C vendor\systeminformer describe --tags --exact-match) -replace '^v', ''
    if ($LASTEXITCODE -or -not $siVersion) { throw 'vendor\systeminformer is not at a release tag' }
    $csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
    New-Item -ItemType Directory -Force release, obj\installer | Out-Null
    $src = (Get-Content installer\Installer.cs -Raw) -replace '@VERSION@', $Version -replace '@SIVERSION@', $siVersion
    [IO.File]::WriteAllText("$PSScriptRoot\obj\installer\Installer.cs", $src, (New-Object Text.UTF8Encoding $false))
    $exe = "release\SystemInformerPlugins-Setup-$Version.exe"
    $res = $Plugin | ForEach-Object { "/resource:bin\$_.dll,plugin.$_.dll" }
    & $csc /nologo /target:winexe /optimize+ /platform:anycpu "/out:$exe" /win32manifest:installer\app.manifest `
        /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll $res obj\installer\Installer.cs
    if ($LASTEXITCODE) { throw "installer build failed ($LASTEXITCODE)" }
    "Built $exe (System Informer $siVersion)"
    $zip = "release\SystemInformerPlugins-$Version.zip"
    Remove-Item $zip -ErrorAction SilentlyContinue
    Compress-Archive -Path ($Plugin | ForEach-Object { "bin\$_.dll" }) -DestinationPath $zip
    "Built $zip"
}
