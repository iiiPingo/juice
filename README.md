<p align="center"><img src="docs/logo.png" width="96" alt="Juice logo"></p>
<h1 align="center">Juice</h1>
<p align="center">Battery levels of your wireless devices, right in the Windows tray.</p>
<p align="center"><a href="../../releases/latest"><b>Download the latest release</b></a></p>

## New in 1.4.0
- Minibar 1.9.0 can show your battery levels in its side menu. Juice keeps a small `status.txt` in its data folder for it (rewritten on every reading, removed when Juice closes).

## New in 1.3.0
- Show device names in the widget with a new setting (tray menu, Show device names).
- Rename any device or hide it from the tray and widget (tray menu, Devices). Your choices are saved.
- A GameSir controller on a 2.4 GHz receiver no longer shows up twice with different batteries.
- A device that reports a garbage name while asleep keeps its real name.
- Diagnostics now lists every controller reading and where it came from.

## New in 1.2.0
- Cleaner widget with modern device icons, level bars, and separate left, right and case readings for earbuds.
- A mouse receiver that reconnects no longer goes grey until Juice restarts.
- Bluetooth mice and controllers now show up beside 2.4 GHz devices of the same type.
- Low battery alerts no longer repeat after a device returns or the PC wakes, and a missed alert is retried.
- Nearby AirPods of the same model are much less likely to show their battery or trigger an alert.
- Fixes for a rare device driver crash, identical receivers, damaged settings, display scale changes, and closing Juice.
## Features
- Battery of your wireless headphones, mice and other devices as **ring icons** in the tray.
- A flyout with a bar for each device, plus a desktop widget.
- **Low-battery alerts** so nothing dies mid-game or mid-call.
- Starts with Windows. Small native C++ app, no ads, no subscription.

**Reads:** any Bluetooth device that shows a battery in Windows Settings (headphones, earbuds, mice, keyboards,
controllers), AirPods and Beats (left, right and case), Logitech mice and keyboards on Unifying / Bolt / Lightspeed
receivers (new in 1.1), SteelSeries Arctis Nova 5 / 5P / 5X, Attack Shark / Ajazz / ATK / VXE 2.4 GHz receivers,
Xbox (Bluetooth) and GameSir controllers. Not yet: Razer, Corsair, HyperX receivers.

Tested with Sony WH-1000XM5 headphones and an Attack Shark X1 mouse.

## Install
1. Download `Juice-1.4.0.zip` from [Releases](../../releases/latest) and unzip it into its own folder.
2. Run `Juice.exe`. If SmartScreen warns, click **More info > Run anyway** (the exe is unsigned).

## Build
C++17, cross-compiled with MinGW-w64 on Linux: `sudo apt-get install g++-mingw-w64-x86-64`, then `./build.sh` -> `out/Juice.exe`. On Windows with Visual Studio 2022: `build.cmd`.
`src/` has the app (tray icons, widget, pollers per device family), `res/` the icon, manifest and version.

## License
PolyForm Noncommercial License 1.0.0: free to use, copy and change for non-commercial purposes, not for selling or other commercial use. See `LICENSE`.
