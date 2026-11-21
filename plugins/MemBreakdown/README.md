# MemBreakdown

Answers "where is my memory going?". Adds a **RAM breakdown** section to System Information (View > System
Information) whose rows add up to the installed RAM, a live version of RAMMap's *Use Counts*.

## What it shows

- **Graphs**, sampled every tick:
  - process private working sets (sum over all processes);
  - other in-use memory (kernel, drivers, mapped files);
  - standby (the file cache);
  - modified pages (waiting to be written);
  - free and zeroed pages.
- **Breakdown** tab. Every physical page is counted by *use* and by *list* from a Superfetch PFN query, the same
  data RAMMap reads:
  - uses: process private, mapped file, shareable, page table, paged and nonpaged pool, system PTE, session
    private, metafile (NTFS metadata), AWE / large pages, driver locked, kernel stack;
  - lists: active, standby, modified, zeroed, free;
  - plus hardware/firmware reserved (installed RAM minus what Windows manages), so the total is the installed RAM.

  Below the total are live rows that are not added to it: standby by priority, resident pool, the system file
  cache working set and commit charge.
- **Processes** tab: the top 100 processes by private working set, with commit (private bytes) and working set.
  Click the Private WS, Commit or Working set header to sort by it. Double-click a process to open its properties.
- **Pool tags** tab: the top 100 kernel pool tags (nonpaged and paged bytes), with driver and description from
  System Informer's `PoolTag.txt`. Pool sizes are allocated bytes. Paged pool can be partly paged out.
- **Cached files** tab: the 64 files holding the most cached pages (active, standby and modified), with the rest
  summed in one row. This is where a large standby cache comes from, for example model files or disk images that
  were read once.

- **Tray icon** (View > Tray icons > *RAM breakdown history*): in-use memory at the bottom with standby (file
  cache) stacked on top, as a share of installed RAM; the empty part is free. The tooltip lists processes, kernel
  and drivers, modified, standby and free. Hovering opens the mini info window on a *RAM breakdown* section:
  in use, standby and free, with processes sorted by private working set.

## How it works

- The page-by-page (PFN) scan runs on its own thread, only while the section is open, every
  `MemBreakdown.ScanIntervalSeconds` seconds (default 30; `0` scans only when you open the section).
- A scan of 128 GB takes about 4 seconds of one CPU core.
- The page data names a file only by a kernel key. While the Cached files tab is showing, each scan also runs a
  private ETW kernel session ("MemBreakdown File Names", file-I/O flag) for a quarter of a second. When it stops,
  the kernel reports a key and path for each file it knows. This adds about 1.5 s per scan. Without that tab
  showing, no trace runs.
- It needs System Informer running as administrator. Without admin, the tab shows the live counters and labels
  the part it cannot explain.
- The status line shows when the last scan ran and how far its standby count is from the kernel's own counter.
  The two are read a few seconds apart, so a small gap is normal.
- After each scan the plugin writes `%APPDATA%\SystemInformer\MemBreakdown.log`: status codes and page counts
  only, no file names.

## Reading the numbers

- **Driver locked** is memory a driver has pinned. GPU drivers pin host memory for CUDA and other compute
  programs. This memory cannot be paged out or given to other programs until the driver releases it.
- **Standby** is cache. Windows hands it to programs on demand, so it counts as available.
- **Modified** pages must be written to disk (pagefile or file) before Windows can reuse them.
- **Commit** is virtual: memory promised to programs, backed by RAM or the pagefile. It is not part of the RAM
  total.

## Check outside System Informer

`tools\memprobe` builds the same sampling code as a console program:

```powershell
tools\build.ps1 -Probe
bin\memprobe.exe out.txt    # run elevated
```

It prints the use × list matrix and the list self-check, without file names.
