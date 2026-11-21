# NvTccGpu

Shows NVIDIA GPUs running in **TCC** (or MCDM) driver mode in System Informer's System Information window:
Tesla/datacenter cards such as P40, P100, V100, T4 and A100, and any GeForce/Quadro switched to TCC.

System Informer's built-in GPU page (and Task Manager) read GPUs through WDDM/D3DKMT, so a GPU in TCC mode is
invisible to them. This plugin reads NVML (`nvml.dll`, installed with the NVIDIA driver) instead.

Each TCC GPU gets its own section with:

- graphs: GPU load, dedicated memory, memory-controller (bandwidth) load, power, temperature
- a panel: memory used/total, power draw/limit, SM and memory clocks, driver model, fan (or "Passive")
- an **Apps** list: every process using the GPU with its PID and GPU memory, largest first; double-click opens the
  process properties
- the GPU and memory graph tooltips name the app holding the most GPU memory at that moment; double-click a graph to
  open that process

Per-app GPU load (%) is not available: NVIDIA does not report per-process utilization on Windows.

Identical cards are numbered (`P40 #0`, `P40 #1`). Temperature follows ExtendedTools' Fahrenheit option.
WDDM GPUs are skipped because System Informer already shows them; set `NvTccGpu.ShowWddmGpus` to `1` to list them too.

Build and install: see the [repository README](../../README.md) (`.\build.ps1 -Plugin NvTccGpu`, `.\install.ps1 -Plugin NvTccGpu`).
