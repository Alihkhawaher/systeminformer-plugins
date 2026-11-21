# systeminformer-plugins

Third-party plugins for [System Informer](https://github.com/winsiderss/systeminformer) (Windows x64).

| Plugin | What it does |
|---|---|
| [GpuMonitor](plugins/GpuMonitor/README.md) | One System Information section per GPU (WDDM and NVIDIA TCC), per-app GPU load and memory, NVIDIA power/temp/clocks. Replaces ExtendedTools' GPU page |
| [DiskMonitor](plugins/DiskMonitor/README.md) | One Disks page: a tile per disk, per-disk speed, active/response time, temperature, per-app I/O (ETW) and SMART health (NVMe, SATA, USB bridges) with Good/Caution/Bad verdict |
| [MemBreakdown](plugins/MemBreakdown/README.md) | "Where is my memory going?": every physical page by use and list (like RAMMap), top processes, pool tags and cached files, with graphs |
| [NvTccGpu](plugins/NvTccGpu/README.md) | TCC/MCDM NVIDIA GPUs only (Tesla P40, A100, ...). Superseded by GpuMonitor; install one or the other |

## Install (easy way)

Download `SystemInformerPlugins-Setup-<version>.exe` from [Releases](https://github.com/Alihkhawaher/systeminformer-plugins/releases)
and run it (it asks for administrator rights). Tick the plugins you want, untick the ones to remove, and click
**Apply**. It closes System Informer, copies the plugins, allows third-party plugins in its settings and starts it
again; **Remove all** undoes it. It checks that your System Informer is the version the plugins were built for. After a build from
source, the same installer is `SystemInformerPlugins-Setup.exe` in the repo root.
Each release also has a `.zip` with the plain DLLs for `C:\Program Files\SystemInformer\plugins`.

## Requirements

- System Informer **4.0.26241.138**. Plugins are built against an exact System Informer version (see below).
- To build: Visual Studio 2022 with the C++ workload, Python 3, git.

## Build

```powershell
git clone --depth 1 --branch v4.0.26241.138 https://github.com/winsiderss/systeminformer.git vendor\systeminformer
python tools\make_sdk.py              # SDK headers + SystemInformer.lib -> sdk\
tools\build.ps1                           # all plugins -> bin\<Name>.dll   (or -Plugin NvTccGpu)
tools\build.ps1 -Probe                    # bin\memprobe.exe: console check of MemBreakdown's sampling code
```

## Make a release

```powershell
.uild.ps1 -Installer -Version 1.2.0     # all plugins + release\SystemInformerPlugins-Setup-1.2.0.exe and .zip
powershell -STA -File installer	est-installer.ps1 release\SystemInformerPlugins-Setup-1.2.0.exe   # checks, changes nothing
```

The installer (`installer\Installer.cs`, WinForms) is built with the .NET Framework 4 `csc.exe` that ships with
Windows, so it needs no SDK to build and nothing extra to run. The DLLs are embedded in it.

## Install / uninstall from a build

```powershell
tools\install.ps1                  # all built plugins (or -Plugin GpuMonitor); UAC prompt, restarts System Informer
tools\install.ps1 -Plugin GpuMonitor -Remove NvTccGpu -SetSetting 'ExtendedTools.EnableGpuMonitor=0'
tools\uninstall.ps1                # all (or -Plugin NvTccGpu)
```

Both take `-SystemInformerDir` (default `C:\Program Files\SystemInformer`) and `-SettingsPath`
(default `%APPDATA%\SystemInformer\settings.json`).

## Things to know

- **Third-party plugins are off by default.** System Informer loads only its own plugins while
  `EnableDefaultSafePlugins` is `1`. the installer (or `tools\install.ps1`) sets it to `0` (backup: `settings.json.bak-plugins`);
  removing them (installer or `tools\uninstall.ps1`) sets it back to `1` once no unsigned plugin is left.
- **The plugins are unsigned.** With one loaded, System Informer's driver runs with reduced protection and shows a
  one-time "unsigned images (likely an unsigned plugin)" warning; some advanced features (details of protected
  system processes, terminating protected processes) are unavailable. Uninstalling restores them.
- **Rebuild after every System Informer update.** System Informer exports its plugin API by **ordinal only**; the
  ordinals are the order of `SystemInformer\SystemInformer.def`, starting at 1001. `make_sdk.py` builds the import
  library from the `.def` of the cloned tag and stops if the export count differs from the installed
  `SystemInformer.exe`. Clone the tag that matches your installed version, then run `make_sdk.py` and `tools\build.ps1`.
- `make_sdk.py` reproduces System Informer's SDK step (header generation and copies) without running its build tools.

GpuMonitor, DiskMonitor and MemBreakdown add **tray icons** with history graphs (View > Tray icons in System
Informer), like its own CPU and I/O history icons.

## Adding a plugin

Create `plugins\<Name>\` with one or more `.c` files, an optional `<Name>.rc` and `resource.h`, and a `README.md`.
`tools\build.ps1` compiles every `.c` in the folder into `bin\<Name>.dll`. Register with `PhRegisterPlugin(L"<Name>", ...)`
in `DllMain`; `plugins\NvTccGpu` is a working example of a System Information section. For tray icons, include
`common\trayicon.h` (on the include path) and register from `GeneralCallbackTrayIconsInitializing`; GpuMonitor,
DiskMonitor and MemBreakdown show how.

## License

MIT. System Informer headers used at build time are MIT licensed by Winsider Seminars & Solutions, Inc.
