# Builds plugins\<Name>\*.c + <Name>.rc into bin\<Name>.dll (x64 Release) against sdk\ (tools\make_sdk.py).
#   .\build.ps1                 # every plugin under plugins\
#   .\build.ps1 -Plugin NvTccGpu
param([string[]]$Plugin)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Test-Path sdk\lib\SystemInformer.lib)) { throw 'sdk\ missing - run: python tools\make_sdk.py' }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vs) { throw 'Visual Studio not found' }
if (-not $Plugin) { $Plugin = (Get-ChildItem plugins -Directory).Name }

$defs = '/DWIN64 /DNDEBUG /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE /DENABLE_RESTRICTED /DCONST_VTABLE /DSI_NO_WPP'
$libs = 'SystemInformer.lib ntdll.lib user32.lib gdi32.lib kernel32.lib comctl32.lib shell32.lib advapi32.lib'

foreach ($name in $Plugin) {
    $dir = "plugins\$name"
    if (-not (Test-Path $dir)) { throw "no plugin folder $dir" }
    $obj = "obj\$name"
    New-Item -ItemType Directory -Force bin, $obj | Out-Null
    $sources = (Get-ChildItem $dir -Filter *.c).FullName | ForEach-Object { "`"$_`"" }
    $inc = "/I sdk\include /I vendor\systeminformer\phnt\include /I `"$dir`""

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
