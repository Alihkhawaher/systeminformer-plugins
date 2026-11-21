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
- System Informer's KSI process create/exit messages (`PhInformerCallback`) are not exported, and ExtendedTools' ETW
  session (disk/file/net flags only) can't be shared: ProcessLog runs its own system-logger session
  (`EVENT_TRACE_SYSTEM_LOGGER_MODE`, unique name + GUID), stopped and replaced at start because install.ps1's kill
  leaves it running.
- ETW real-time events are NOT in order across CPUs: a reused PID's Start can arrive before the End of its previous
  owner, and a process's End before its own Start. Match processes by PID + UniqueProcessKey, never by PID alone.
- Kernel MOF payload layouts can be read without admin: `Get-CimClass -Namespace root\wmi -ClassName
  Process_V4_TypeGroup1` (field order = WmiDataId). Without `PROCESS_TRACE_MODE_RAW_TIMESTAMP` the event TimeStamp
  is a FILETIME.
- System Informer integer settings are hex strings (`L"20"` = 32).
- Reading a log System Informer has open from PowerShell needs FileShare ReadWrite (`[IO.File]::Open(p,'Open',
  'Read','ReadWrite,Delete')`); `ReadAllLines` fails. Test via the plugin's files, never paste real command lines.
- In a worktree, `vendor` may be a junction to the main checkout's clone: remove it with `cmd /c rmdir vendor`
  before deleting the worktree so a recursive delete can't follow it.
- Sibling sessions' `install.ps1` runs restart System Informer, so whatever ProcessLog.dll is installed at that
  moment runs (and rewrites its log). When judging a log, tie lines to the build that wrote them (startup blocks).
- ProcessLog check without the window: `plugins\ProcessLog	est-processlog.ps1` (status, log health, test row).
