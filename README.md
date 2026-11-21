# systeminformer-plugins

Third-party plugins for [System Informer](https://github.com/winsiderss/systeminformer) (Windows x64).

| Plugin | What it does |
|---|---|
| [NvTccGpu](plugins/NvTccGpu/README.md) | Shows NVIDIA GPUs in TCC/MCDM mode (Tesla P40, A100, ...) in System Information, via NVML |
| [ProcessLog](plugins/ProcessLog/README.md) | Logs every process start and exit from ETW kernel events, including processes that live only milliseconds; searchable list, CSV export (Tools > Process log) |

## Requirements

- System Informer **4.0.26241.138**. Plugins are built against an exact System Informer version (see below).
- To build: Visual Studio 2022 with the C++ workload, Python 3, git.

## Build

```powershell
git clone --depth 1 --branch v4.0.26241.138 https://github.com/winsiderss/systeminformer.git vendor\systeminformer
python tools\make_sdk.py              # SDK headers + SystemInformer.lib -> sdk\
.\build.ps1                           # all plugins -> bin\<Name>.dll   (or -Plugin NvTccGpu)
```

## Install / uninstall

```powershell
.\install.ps1                  # all built plugins (or -Plugin NvTccGpu); UAC prompt, restarts System Informer
.\uninstall.ps1                # all (or -Plugin NvTccGpu)
```

Both take `-SystemInformerDir` (default `C:\Program Files\SystemInformer`) and `-SettingsPath`
(default `%APPDATA%\SystemInformer\settings.json`).

## Things to know

- **Third-party plugins are off by default.** System Informer loads only its own plugins while
  `EnableDefaultSafePlugins` is `1`. `install.ps1` sets it to `0` (backup: `settings.json.bak-plugins`);
  `uninstall.ps1` sets it back to `1` once none of these plugins is left.
- **The plugins are unsigned.** With one loaded, System Informer's driver runs with reduced protection and shows a
  one-time "unsigned images (likely an unsigned plugin)" warning; some advanced features (details of protected
  system processes, terminating protected processes) are unavailable. Uninstalling restores them.
- **Rebuild after every System Informer update.** System Informer exports its plugin API by **ordinal only**; the
  ordinals are the order of `SystemInformer\SystemInformer.def`, starting at 1001. `make_sdk.py` builds the import
  library from the `.def` of the cloned tag and stops if the export count differs from the installed
  `SystemInformer.exe`. Clone the tag that matches your installed version, then run `make_sdk.py` and `build.ps1`.
- `make_sdk.py` reproduces System Informer's SDK step (header generation and copies) without running its build tools.

## Adding a plugin

Create `plugins\<Name>\` with one or more `.c` files, an optional `<Name>.rc` and `resource.h`, and a `README.md`.
`build.ps1` compiles every `.c` in the folder into `bin\<Name>.dll`. Register with `PhRegisterPlugin(L"<Name>", ...)`
in `DllMain`; `plugins\NvTccGpu` is a working example of a System Information section.

## License

MIT. System Informer headers used at build time are MIT licensed by Winsider Seminars & Solutions, Inc.
