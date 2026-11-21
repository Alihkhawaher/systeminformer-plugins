# systeminformer-plugins: working notes

Public repo (github.com/Alihkhawaher/systeminformer-plugins). Never commit personal data: machine names, user
paths, emails, client names. Commit author: `alihkhawaher <7887710+Alihkhawaher@users.noreply.github.com>`.
Pushes to this public repo need the owner's explicit OK each time.

## Layout

- `plugins\<Name>\` one folder per plugin (sources, `.rc`, `resource.h`, `README.md`). Keep the root README table current.
- `tools\make_sdk.py` builds `sdk\` (headers + ordinal import lib) from `vendor\systeminformer` (git clone of the
  tag matching the installed System Informer) and the installed `SystemInformer.exe`.
- `build.ps1` / `install.ps1` / `uninstall.ps1` take `-Plugin`; extend them, don't add per-plugin copies.

## Lessons (don't repeat)

- Do not run System Informer's own build scripts (`build\*.cmd`, CustomBuildTool): running downloaded repo tooling is
  blocked for the coding agent. `make_sdk.py` reproduces the SDK step instead.
- `SystemInformer.exe` exports by ordinal only (NONAME). Ordinal = 1001 + position of the line in
  `SystemInformer.def` (CustomBuildTool `ExportDefinitions`). dumpbin shows no names.
- Plugins only load when `EnableDefaultSafePlugins` = 0 (settings.json). Unsigned plugin -> KSI "unsigned images"
  reduced protection; tell the user before installing a new plugin.
- System Informer runs elevated with `HideOnClose=1`: closing the window only hides it, and a non-elevated shell can't
  see its modules. `install.ps1` stops it (elevated) before editing settings, else it overwrites them on exit.
  Verifying a plugin loaded needs the user to look (or an elevated check).
- `PhGetIntegerSetting` raises an exception for a setting that doesn't exist (e.g. another plugin's setting while that
  plugin isn't loaded): check with `PhFindPlugin2` and guard with `__try`.
- `vcvars64.bat` prints a harmless "vswhere.exe is not recognized" on stderr; build.ps1 sends it to nul.
- NVML: `nvmlDeviceGetDriverModel` current 0=WDDM, 1=TCC (WDM), 2=MCDM. P40 has no fan sensor (GetFanSpeed fails).
