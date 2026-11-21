# GpuMonitor

One System Information section **per GPU**: each card gets its own entry in the left-hand list, with its own
graphs and its own app list. It covers both normal (WDDM) GPUs and NVIDIA GPUs in TCC/MCDM mode, which Windows and
System Informer's built-in GPU page cannot see. It replaces ExtendedTools' combined GPU page; it also supersedes
[NvTccGpu](../NvTccGpu/README.md), so install one or the other.

| | WDDM GPU (e.g. GeForce) | NVIDIA TCC/MCDM GPU (e.g. Tesla P40) |
|---|---|---|
| GPU load | busiest engine, like Task Manager (Windows GPU counters) | NVML |
| Dedicated memory | Windows counters | NVML |
| Shared memory | Windows counters | - |
| Memory-controller load, power, temperature, clocks, fan | NVML (NVIDIA only) | NVML |
| Apps: GPU load % | yes (Windows counters) | not available (NVIDIA doesn't report it on Windows) |
| Apps: GPU memory | yes | yes (NVML) |

- The **Apps** list shows every process using the GPU, busiest first (largest memory first on TCC). Double-click a
  row for the process properties.
- GPU and memory graph tooltips name the busiest app at that moment (largest memory holder on TCC); double-click a
  graph to open that process.
- **Tray icons** (View > Tray icons), per GPU: *GPU history*, *GPU memory history* and *GPU temperature
  history* (NVIDIA only). The tooltip shows load, VRAM used of total, temperature and power, and the
  busiest app (largest VRAM holder on TCC).
  Hovering an icon opens System Informer's mini info window on a *GPU: <name>* section: load and VRAM, and the
  apps sorted by GPU load (by VRAM on TCC).
- Identical cards are numbered (`P40 #1`, `P40 #2`). Temperature follows ExtendedTools' Fahrenheit option.

Sources: D3DKMT (adapter names, PCI bus, memory sizes), the Windows "GPU Engine", "GPU Process Memory" and
"GPU Adapter Memory" performance counters, and NVML (`nvml.dll`, installed with the NVIDIA driver). NVIDIA WDDM
cards are matched to their NVML device by PCI bus number.

## Install

```powershell
tools\build.ps1 -Plugin GpuMonitor
tools\install.ps1 -Plugin GpuMonitor -Remove NvTccGpu -SetSetting 'ExtendedTools.EnableGpuMonitor=0'
```

`ExtendedTools.EnableGpuMonitor=0` turns off ExtendedTools' own GPU page (and its per-process GPU columns in the
process list). Set it back to `1` to get them back.
