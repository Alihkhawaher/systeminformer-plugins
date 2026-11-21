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
- WDDM per-process GPU data: PDH wildcard counters `\GPU Engine(*)\Utilization Percentage` (instance
  `pid_<pid>_luid_0x<hi>_0x<lo>_phys_<n>_eng_<n>_engtype_<type>`), `\GPU Process Memory(*)\Dedicated Usage`,
  `\GPU Adapter Memory(*)\Dedicated|Shared Usage`. Use `PDH_FMT_NOCAP100`; collect once at load (rate counter).
  Adapter load = busiest engine (sum over pids per engine), per-app load = that app's busiest engine (Task Manager).
- D3DKMT: GpuMonitor declares the few structs it needs (values from ExtendedTools' vendored `d3dkmt\d3dkmthk.h`) and
  loads the functions from gdi32 at runtime; don't add ExtendedTools' folder to the include path (its `resource.h`).
- install.ps1 `-Remove <Plugin>` deletes replaced plugin DLLs and `-SetSetting 'Name=Value'` edits existing
  settings.json entries in the same elevated run (one UAC prompt, one restart).
- Don't `#include <winioctl.h>` in a plugin: phdk.h already brings it in, and a second include redefines its GUIDs
  (C2374). `<ntddscsi.h>` is fine.
- Not every phlib global is exported to plugins (e.g. `PhWhitespaceStringRef`, `PhSpaceStringRef` link-fail as
  undeclared): define a local `PH_STRINGREF` instead. Check `SystemInformer.def` before relying on one.
- SMART code is tested outside System Informer with `plugins\DiskMonitor\test\smarttest.c` (see its README); NVMe
  reads work unelevated, ATA/USB need admin. Never send a vendor USB-bridge command to an unlisted bridge.
- Per-app, per-disk I/O: own ETW session with `EVENT_TRACE_SYSTEM_LOGGER_MODE` + `EVENT_TRACE_FLAG_DISK_IO` (DiskIo
  opcode 10/11, `IssuingThreadId` -> process via OpenThread/GetProcessIdOfThread, cached). ExtendedTools' session
  data isn't reachable from other plugins. phnt already defines a `DiskIoGuid` symbol: use another name.
- `PluginCallbackUnload` runs on normal exit only; a force-killed System Informer leaves an ETW session running, so
  stop-and-recreate it by name at start. `logman query -ets` lists running sessions.
- Many disks crammed the sidebar (one section each): use one section with tiles at the top of the page instead.
