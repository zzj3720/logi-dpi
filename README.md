# logi-dpi

A tiny, instant-launch native Windows DPI controller for modern Logitech LIGHTSPEED gaming mice (**PRO X2 SUPERSTRIKE**, **PRO X Superlight 2**, etc.) written in **[Pion](https://github.com/AFK-surf/pion)** with **invar**'s Liquid Glass components (`lg`) and direct **Win32 USB HID++ 2.0** (`0x2202` Extended Adjustable DPI).

No Logitech G HUB. No "Loading resources…" infinite spinner. No background services. No Electron.

![Preview](assets/preview.jpg)

## Features

- **Direct USB HID++ 2.0 (`0x2202`)**: Reads and writes hardware DPI directly over the LIGHTSPEED receiver (`VID_046D&PID_C54D` / `C547` / `C539`) with zero G HUB dependency.
- **Auto Host-Mode Handshake (`0x8100`)**: Automatically switches onboard-locked mice to host mode before writing live DPI so firmware accepts changes immediately.
- **Interactive Ticked Slider & Presets**: Smoothly drag or click along the 200–4000 DPI scale (50 DPI steps), fine-tune with `-50 / +50`, or jump to `400 / 800 / 1200 / 1600 / 3200`.
- **Lift-Off Distance (LOD)**: Switch between Low (`1`), Medium (`2`), and High (`3`) LOD directly on the sensor.
- **Live Battery (`0x1004`)**: Reads Unified Battery percentage directly from the mouse.
- **Auto-Save & Sleep/Wake Guard**: Saves your DPI & LOD to `config.ini`, minimizes to the Windows system tray on close, automatically restores your DPI if the mouse wakes from sleep or reconnects, and includes a one-click Windows autostart (`--tray`) toggle.
- **Liquid Glass UI**: one wallpaper with single-layer glass controls (slider, ±50 stepper, preset chips, LOD segmented control, toggle) from invar's `lg`, following its design laws (no glass on glass, concentric corners, ≥ 4.5:1 text); light or dark as Windows is. GPU-drawn by invar's Windows host (Direct3D 11 + DirectWrite).
- **No HID I/O on the UI thread**: a background thread reads the mouse every 30 s or on Refresh; the window only takes the cached values.

## Quick Start (Windows x64)

1. Download `MouseDPI-win-x64.zip` from [Releases](https://github.com/zzj3720/logi-dpi/releases).
2. Extract `MouseDPI.exe` and `mouse.png` to the same folder.
3. Run `MouseDPI.exe` (close G HUB if it is holding the HID++ interface).

## Protocol Notes (HID++ 2.0)

| Feature ID | Name | Usage |
|---|---|---|
| `0x0000` | `IRoot` | Resolve feature indices dynamically |
| `0x1004` | `UNIFIED_BATTERY` | Read live battery percentage (`fn=1`) |
| `0x2202` | `EXTENDED_ADJUSTABLE_DPI` | Read (`fn=5`) & write (`fn=6`) X/Y DPI and LOD |
| `0x8100` | `ONBOARD_PROFILES` | Read mode (`fn=2`) & set host mode (`fn=1`, `0x02`) |

## Building from Source

Layout: `pion/packages/dpi_ui` (the window, portable), `pion/cmd/mousedpi` (Windows app + `hidpp_win.c`),
`pion/cmd/shots` (the window drawn offscreen on macOS with a stand-in mouse, for previews). `tools/deps.sh` fetches the
pinned [invar](https://github.com/AFK-surf/invar) into `deps/invar`.

**Windows** (Git Bash): needs a `pionc.exe` + `qbe.exe` bootstrapped from the Pion repo (MinGW-w64 gcc, as its
`scripts/test-windows.sh`), the [LLVM release](https://github.com/llvm/llvm-project/releases) for
`x86_64-pc-windows-msvc`, and Visual Studio Build Tools (MSVC libraries + Windows SDK). invar's Windows host is written
for MSVC, so it is compiled with clang for the MSVC target; `tools/win-cc.c` adapts pionc's GCC-style link flags.

```sh
PIONC=…/pionc.exe PION_QBE=…/qbe.exe PION_SRC=…/pion [LLVM_BIN=…/llvm/bin] sh tools/build-win.sh   # -> out/MouseDPI.exe
```

**macOS preview**: `sh tools/deps.sh && pionc build --release pion/cmd/shots -o dpi_shots && DPI_SHOTS=out DPI_PICTURE=$PWD/assets/mouse.png ./dpi_shots`

## License

MIT © 2026 zzj3720
