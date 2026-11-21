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
- NVML per-process: `nvmlDeviceGetComputeRunningProcesses_v3/_v2` (4-field struct) or the original (2-field) gives
  pid + VRAM, also in TCC. Per-process load % (`nvidia-smi pmon`, process utilization) is "Not supported" on Windows.
- install.ps1: the old DLL can stay locked a few seconds after System Informer is killed (copy retries 20 s), and
  System Informer is restarted in `finally` so a failed install never leaves it stopped.
- PowerShell 5.1 `Start-Process -Wait` also waits for child processes: the elevated installer's wait never returned
  because it had started System Informer. Use `-PassThru` + `$p.WaitForExit()`.
- MemBreakdown PFN scan: `SystemSuperfetchInformation` with `SuperfetchMemoryRangesQuery` (V1 fails on Win10 19045,
  V2 works) then `SuperfetchPfnQuery` in 64K-PFN chunks (PFNs go in `PageFrameIndex`, `RequestFlags` 1). Needs admin
  + SeProfileSingleProcessPrivilege (System Informer enables it). 128 GB = ~4 s; run it off the UI/provider thread.
- Zero/free pages report UseDescription 0 (= process private): count them by list only. The scan's list totals match
  `SystemMemoryListInformation` within ~1 GB (memory moves during the scan).
- `SystemMemoryListInformation` works without admin on 19045. `SYSTEM_PERFORMANCE_INFORMATION.ResidentSystemDriverPage`
  reads 0xFFFFFFFF (unusable); ResidentSystemCodePage reads 0.
- PFN file pages' `u2.FileObject` is NOT a FILE_OBJECT: probed against all 1.8M system handles, no key matched any
  handle's object (keys look like control-area addresses). File names need a kernel driver; don't retry handle matching.
- Not in SystemInformer.def (not callable from plugins): `PhDereferenceObjects`, `PhPerfInformation`, `PhGetPhysicallyInstalledSystemMemory`.
  `PhEnumProcessItems` returns VOID. SI integer settings are hex strings ("1e" = 30).
- Dialog procs get WM_SIZE before WM_INITDIALOG: guard globals/layout managers until init ran.
- To test privileged code without System Informer: `build.ps1 -Probe`, then run `bin\memprobe.exe out.txt` elevated
  (`Start-Process -Verb RunAs -PassThru`, `WaitForExit`).
