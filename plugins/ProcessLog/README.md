# ProcessLog

A log of every process that ran: when it started and exited, how long it lived, who launched it, its command
line, user, session and exit code. Very short-lived processes are included. A process that lives 40 ms (e.g.
`cmd /c exit 3`) is far too short for System Informer's 1-second refresh to see, but it is still logged.

**Tools > Process log** opens the list.

Columns: Start, Exit, Duration, Process, PID, Parent, Exit code, User, Session, Image path, Command line, Notes

- **Search** box: matches the name, image path, command line, parent, user, or an exact PID / parent PID.
- **Only short-lived (< 1 s)** and **Only running** filters. Short-lived rows are highlighted.
- Click a column header to sort. Newest first by default.
- **Double-click** (or Enter) shows all details of a row, with a Copy button.
- **Copy** / Ctrl+C copies the selected rows (tab-separated, with a header). Ctrl+A selects all.
- **Export CSV...** writes the rows currently shown (filters applied) as UTF-8 CSV.
- The status line shows whether capture is running and how many ETW events were lost.

## How it captures

The plugin runs its own ETW **system-logger session** (`SiProcessLogSession`, Windows 8+), with the kernel `PROCESS` and
`IMAGE_LOAD` flags. The kernel's Process Start/End events carry the PID, parent PID, session, user SID, command
line and exit status. The first image load of a new process gives its full image path. Everything comes from the
event payloads and nothing is read from the live process. Events arrive up to about 1 s late, and by then a short
process is long gone.

Why not reuse an existing session: System Informer's own kernel process notifications (the KSI informer) are not
exported to plugins. ExtendedTools' kernel session carries only disk, file and network events, and other plugins
can't reach its consumer. A separate system-logger session is cheap and leaves both of them untouched. A
session left running after System Informer is killed is stopped and replaced at the next start.

Notes column:

| Note | Meaning |
|---|---|
| short-lived | lived less than 1 second |
| started before logging | already running when System Informer started; start = the process's create time; listed once it exits |
| start not seen / exit not seen | System Informer was not running at that moment |
| created by X (pid) | the process that called CreateProcess differs from the parent shown (parent-process spoofing, e.g. elevation) |

## Files

- `%APPDATA%\SystemInformer\ProcessLog.tsv`: the log, UTF-8, tab-separated, one line per start (`S`) and per
  exit (`E`). It's written as events arrive, so it survives System Informer being killed. At the size cap it
  is renamed to `ProcessLog.tsv.1` (replacing the older one) and a new file starts. Both files are loaded at start.
- `%APPDATA%\SystemInformer\ProcessLog.status.txt`: capture state and event counters, refreshed every ~10 s.

**The log holds command lines**, which can contain secrets that programs were given on the command line. It
stays in your profile; uninstalling the plugin does not delete it.

## Settings (settings.json, hex values like every System Informer integer)

| Setting | Default | |
|---|---|---|
| `ProcessLog.MaxLogFileMB` | `20` (32 MB) | size of each of the two log files |
| `ProcessLog.MaxEntries` | `c350` (50,000) | processes kept in memory and shown |

A busy development machine can start several hundred processes a minute, so the defaults hold a few hours of
history there and much more on a quiet one.

## Limits

- Processes that start and exit while System Informer is not running are not logged.
- Per-process ETW events can be dropped under extreme load. The status line shows the lost-event count.
