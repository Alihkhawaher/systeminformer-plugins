# DiskMonitor

One **Disks** section in System Information for all physical disks (found automatically, no setup):

- **Tiles across the top**, one per disk: name, health, temperature and a read/write mini graph. Click a tile to
  select that disk; the choice is remembered (`DiskMonitor.SelectedDisk`).
- **Graphs** of the selected disk: read/write speed (two lines), active time, average response time, temperature.
- **Apps**: processes doing I/O on the selected disk right now (read/s, write/s); double-click for properties.
  Read/write tooltips (tiles and graph) name the busiest app at that moment.
- **Tray icons** (View > Tray icons): *Disk history: all disks*, and a *Disk history* read/write graph per disk. The tooltip shows health, read/write speed, active time, temperature and the busiest
  app on that disk. Hovering opens the mini info window on a *Disks* or *Disk N (X:)* section with the apps sorted
  by read + write speed.

Per-app I/O comes from a private ETW system-logger session `SiDiskMonitorSession` with only disk-I/O events
(Windows 8+, runs beside ExtendedTools' `SiKernelTraceSession`; needs administrator). It is stopped when System
Informer exits normally; if System Informer is killed, the next start stops and recreates it.

**Panel:**
- **Health:** Good / Caution / Bad / Unknown with the reasons, using the same rules as the remote support agent's
  health check:
  - wear >= 80 % caution, >= 95 % bad; temperature >= 65 °C caution, >= 75 °C bad
  - NVMe: the drive's own critical-warning flags, media/data-integrity errors, minutes above critical temperature
  - ATA: any attribute at or below its failure threshold is bad; any reallocated, pending or uncorrectable
    sectors is caution
  - average response time over the last SMART interval >= 20 ms caution, >= 100 ms bad (at least 10 I/Os)
  - free space on the disk's volumes < 10 % caution, < 5 % bad; system drive < 10 GB caution, < 3 GB bad
- temperature, power-on hours, power cycles, wear, lifetime data written / read
- the disk's drive letters with free space
- the full SMART table: NVMe SMART / Health log fields, or ATA attributes (ID, name, current, worst, threshold, raw)

**How SMART is read** (ported from the remote support agent; documented IOCTLs, no driver):
- NVMe: `IOCTL_STORAGE_QUERY_PROPERTY`, NVMe log page 02h.
- SATA: `SMART_RCV_DRIVE_DATA`, READ ATTRIBUTES (D0h) and READ THRESHOLDS (D1h).
- USB enclosures: `IOCTL_SCSI_PASS_THROUGH` - SATA disks via SAT ATA PASS-THROUGH (16, then 12); NVMe disks via the
  JMicron / Realtek / ASMedia command, sent **only** to a bridge whose VID:PID is known for it (smartmontools
  `drivedb.h` lists). A bridge reply is only believed if it is plausible (SATA sector checksum, NVMe field ranges).
  Only read commands are ever sent.

SMART is read once a minute per disk (`DiskMonitor.SmartIntervalSeconds`, default 60), one disk per refresh, and
**never from a hard disk that is spun down** (`GetDevicePowerState`), so the monitor doesn't keep sleeping drives
awake; the panel then says "asleep" and keeps the last reading. Virtual disks are hidden
(`DiskMonitor.ShowVirtualDisks` = 1 to show them). Temperature follows ExtendedTools' Fahrenheit option.

## Test the SMART reader without System Informer

```powershell
# from the repo root, in a Developer prompt (vcvars64)
cl /nologo /std:clatest /W3 /utf-8 /DWIN64 /DUNICODE /D_UNICODE /DSI_NO_WPP /I sdk\include /I vendor\systeminformer\phnt\include /I plugins\DiskMonitor /Foobj\ /Feobj\smarttest.exe plugins\DiskMonitor\test\smarttest.c plugins\DiskMonitor\smart.c setupapi.lib cfgmgr32.lib ntdll.lib
obj\smarttest.exe 4 nvme      # SATA (sata) and USB (usb) disks need an elevated prompt
```
