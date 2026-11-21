# SmartPanel

Streams live readings from System Informer to a cheap 3.5" USB "PC status monitor" sensor panel: the kind sold
as "USB PC Status Monitor Screen / Mini Hardware Sensor Panel", which shows up as a HID device **VID 0483, PID
0065** (product string "llTechCo.,Ltd", vendor llhmi.com). With this plugin you don't need the vendor's Windows
app.

## How the panel works

The panel has **no framebuffer**: the PC can't draw on it.
- **Theme:** a theme (`img.dat`, holding the background and number widgets bound to channels) is uploaded to its
  flash once over YMODEM-over-HID. Making and uploading themes is not part of this plugin; see "Themes" below.
- **Live values:** after that, the host sends only `(channel, value)` pairs, as 64-byte output reports
  `[0x02][count][channel][value BE16]...` with up to 20 pairs each. A clock report
  `[0x03][0x01][0x15][yy][mm][dd][HH][MM][SS][weekday][0x64]` goes once a minute.

The plugin does just this second part, once a second, from a background thread. If the panel is unplugged it
reconnects automatically.

## Layout file

Which reading goes to which channel depends on the theme, so it comes from a text file:
- **Setting:** `SmartPanel.LayoutFile`.
- **Default:** `%APPDATA%\SystemInformer\SmartPanel.txt`.
- **Reloading:** the plugin reloads the file whenever it changes.

```text
# field              mode    channels
gpu:P40:temp         digits  1 2
gpu:P40:power        digits  7 8 9
cpu                  digits  18 19
ram_gb               value   22
```

| Field | Reading |
|---|---|
| `cpu` | CPU usage % |
| `ram_pct`, `ram_gb` | Physical memory in use (% and GB) |
| `net_down_mbs`, `net_up_mbs` | Network throughput in MB/s, summed over the physical interfaces that are up |
| `gpu:<name part>:temp` | NVIDIA GPU temperature in C. `<name part>` is matched against the NVML name, e.g. `P40`, `1070`, `RTX 4090` |
| `gpu:<name part>:load`, `mem_load` | GPU load and memory-controller load % |
| `gpu:<name part>:vram_gb`, `vram_pct` | Dedicated memory in use |
| `gpu:<name part>:power`, `clock` | Power draw in W, SM clock in MHz |

GPU readings come from NVML, so TCC cards such as the Tesla P40 work too, just as in NvTccGpu and GpuMonitor.

Modes:
- **`value`** sends the whole number to one channel.
- **`digits`** sends one decimal digit per channel, most significant first, and clips the value to that width
  (e.g. 2 channels gives 0-99). Use it with themes that draw every digit as its own widget. Some units mirror
  portrait themes in hardware, and a pre-mirrored theme needs this, because the firmware's left-to-right digit order
  would otherwise come out reversed.

## Themes and theme uploads

Build and upload themes with an external tool, e.g. the Python projects
[VPROFi/hw_monitor](https://github.com/VPROFi/hw_monitor),
[Agentry433/turing-smart-screen-python-HIDdev2](https://github.com/Agentry433/turing-smart-screen-python-HIDdev2)
or [marquimRcc/screen-monitor-display](https://github.com/marquimRcc/screen-monitor-display).

**While uploading, the tool must hold the mutex `Local\SmartPanel.Flash`.** The plugin then closes the device
and sends nothing until the mutex is gone; otherwise its reports would land in the middle of the YMODEM stream.
In Python:

```python
mutex = ctypes.windll.kernel32.CreateMutexW(None, True, r"Local\SmartPanel.Flash")
time.sleep(2.5)            # let the plugin's current tick finish
...upload...
ctypes.windll.kernel32.CloseHandle(mutex)
```

## Settings

| Setting | Default | |
|---|---|---|
| `SmartPanel.LayoutFile` | empty = `%APPDATA%\SystemInformer\SmartPanel.txt` | Environment variables are expanded |
| `SmartPanel.IntervalMs` | `3e8` (1000 ms, hex like every System Informer integer setting) | Minimum 250. Read at start |

Build and install: see the [repository README](../../README.md) (`tools\build.ps1 -Plugin SmartPanel`,
`tools\install.ps1 -Plugin SmartPanel`).
