# logi-dpi

A tiny, instant-launch native DPI controller for modern Logitech LIGHTSPEED and wired gaming mice (**PRO X2 SUPERSTRIKE**, **PRO X Superlight 2**, etc.) on **macOS**, **Linux**, and **Windows**, written in **[Pion](https://github.com/AFK-surf/pion)** with **[invar](https://github.com/AFK-surf/invar)**'s Liquid Glass components (`lg`) and a shared **USB HID++ 2.0** protocol core (`0x2202` Extended Adjustable DPI).

No Logitech G HUB. No "Loading resources…" infinite spinner. No background services. No Electron.

![Preview](assets/preview.jpg)

## Features

- **Cross-Platform Native GPU UI**: Built on `invar@0947166` with Apple Two-Layer Liquid Glass (`<lg::PageShell>` solid content layer + floating glass top bar islands) rendered natively via Metal (macOS), EGL/GLES3 + GTK4 (Linux), and Direct3D 11 + DirectWrite (Windows).
- **Direct USB HID++ 2.0 (`0x2202`)**: Shared `hidpp_core.c` protocol engine over native OS transports:
  - **macOS**: IOKit `IOHIDManager` (`hidpp_macos.m`), LaunchAgent autostart, native traffic lights & menu bar status item.
  - **Linux**: `/dev/hidraw` + full HID Report Descriptor parser (`hidpp_linux.c`), XDG autostart, StatusNotifier tray support, and udev `uaccess` rule (`assets/70-logi-dpi.rules`).
  - **Windows**: Win32 HID + `OVERLAPPED` I/O (`hidpp_win.c`), system tray, sleep/wake guard, and `HKCU\...\Run` autostart.
- **Dynamic Device Name (`0x0005`) & Multi-Endpoint Probing**: Enumerates LIGHTSPEED receivers, direct wired Logitech mice, and Bolt/Unifying multi-receivers until `0x2202` is found.
- **Auto Host-Mode Handshake (`0x8100`)**: Automatically switches onboard-locked mice to host mode before writing live DPI.
- **Interactive Ticked Slider & Presets**: Drag along the 200–4000 DPI scale (50 DPI steps), fine-tune with `-50 / +50`, or jump to `400 / 800 / 1200 / 1600 / 3200`.
- **Lift-Off Distance (LOD) & Live Battery (`0x1004`)**: Switch between Low / Medium / High LOD and display a responsive battery badge (switches to warning tone at ≤ 20%).
- **No HID I/O on the UI Thread**: Background worker and watcher threads handle all HID++ I/O and deliver atomic snapshots to the UI.

## Protocol Notes (HID++ 2.0)

| Feature ID | Name | Usage |
|---|---|---|
| `0x0000` | `IRoot` | Resolve feature indices dynamically |
| `0x0005` | `DEVICE_NAME` | Read live device model name |
| `0x1004` | `UNIFIED_BATTERY` | Read live battery percentage (`fn=1`) |
| `0x2202` | `EXTENDED_ADJUSTABLE_DPI` | Read (`fn=5`) & write (`fn=6`) X/Y DPI and LOD |
| `0x8100` | `ONBOARD_PROFILES` | Read mode (`fn=2`) & set host mode (`fn=1`, `0x02`) |

## Repository Layout

- `pion.toml`: Pion workspace root (`dpi_ui`, `mousedpi`, `dpi_shots`)
- `pion/packages/dpi_ui`: Portable Liquid Glass UI package & `ui::Tester` + `lg::assert_apple_ui` tests
- `pion/cmd/mousedpi`: Native desktop app (`main.pion`, `hidpp_core.c`, `hidpp_macos.m`, `hidpp_linux.c`, `hidpp_win.c`)
- `pion/cmd/shots`: Offscreen PNG & 60fps MP4 interaction capture via `invar/packages/film` (macOS Metal & Linux `llvmpipe`)
- `assets/70-logi-dpi.rules`: Linux udev rule (`70 < 73` so `TAG+="uaccess"` runs before `73-seat-late.rules`)
- `assets/com.zzj3720.MouseDPI.desktop`: Linux desktop entry

## Building from Source

Fetch the pinned `invar` dependency first (all build scripts also call this automatically):

```sh
sh tools/deps.sh
```

### macOS

```sh
PIONC=/path/to/pionc2 sh tools/build-macos.sh   # -> out/MouseDPI and out/MouseDPI.app
```

> Note: Grant **Input Monitoring** permission to `MouseDPI.app` in macOS System Settings if prompted by IOKit.

### Linux

Install system development libraries (Debian/Ubuntu):

```sh
sudo apt-get install -y libgtk-4-dev libsoup-3.0-dev libepoxy-dev
```

Build and install (binary, desktop entry, and `/etc/udev/rules.d/70-logi-dpi.rules`):

```sh
PIONC=/path/to/pionc2 sh tools/build-linux.sh    # -> out/mousedpi
sh tools/install-linux.sh                        # installs binary + udev rules + reloads udevadm
```

### Windows (Git Bash)

Requires `pionc.exe` + `qbe.exe`, LLVM (`x86_64-pc-windows-msvc`), and Visual Studio Build Tools (MSVC + Windows SDK):

```sh
PIONC=…/pionc.exe PION_QBE=…/qbe.exe PION_SRC=…/pion [LLVM_BIN=…/llvm/bin] sh tools/build-win.sh   # -> out/MouseDPI.exe
```

### Testing & Offscreen Previews

```sh
# Workspace type check & dpi_ui unit tests
"$PIONC" check --json
"$PIONC" test --json pion/packages/dpi_ui

# Offscreen PNG + 60fps MP4 capture (macOS Metal or Linux llvmpipe)
"$PIONC" build --release pion/cmd/shots -o out/dpi_shots
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe DPI_SHOTS=out/shots DPI_PICTURE="$PWD/assets/mouse.png" ./out/dpi_shots
```

## License

MIT © 2026 zzj3720
