# Juice

Battery levels of wireless devices as ring icons in the Windows tray, with a flyout and a desktop widget. Native C++17 (not .NET), no dependencies.

## Build and test
- Cross-compiled on Linux with MinGW-w64: `sudo apt-get install g++-mingw-w64-x86-64`, then `./build.sh` writes `out/Juice.exe`.
- `res/juice.rc` holds the version resource (VERSIONINFO), icon and manifest. Bump the version there every release. Minibar compares it with the GitHub tag; without a version resource Minibar shows "No version number" and offers no update.
- Release zip: `Juice-x.y.z.zip` with `Juice/Juice.exe` and `README.txt`.
- There is no Windows runtime in the Linux workspace (Wine doesn't start), so hardware behaviour can't be tested here.

## Layout
- `src/main.cpp` tray, flyout, widget. `src/devices.cpp` and `src/poller.cpp` device list and polling. `src/vendors.cpp` Logitech HID++ 2.0 (Unifying/Bolt/Lightspeed receivers), `src/airpods.cpp`, `src/gamepad.cpp`, `src/hid.cpp` other families. `src/icons.cpp` ring icons.
- Bluetooth batteries come from the Windows device property, so any Bluetooth device that shows a battery in Windows Settings works.

## Gotchas
- Only add a poller for a device family if the protocol is known to work. Razer was skipped because its battery command may need Razer's driver. Prefer reporting over guessing.
- `Juice.exe --exit` closes it cleanly (message `WM_APP + 4` to the message window). It must never start Juice. Minibar's Stop and updates rely on it.
- Settings and the log (`settings.ini`, `juice.log`) live next to the exe or in `%APPDATA%\Juice`. Never touch them.
- The supported-devices note belongs in Minibar's card, not in Juice's widget.

## Releasing
When a change is ready to ship, leave it fully done, so the release can go out without rebuilding or repackaging:
1. Bump the version (see Build and test), add the "what's new" to the README and to `README.txt` in the zip (a "NEW IN x.y.z" section at the top, plain words), and run the tests.
2. Build the exe from the final source, last, after the final change. The exe you leave is the one that ships. Copy it to `C:\Claude Projects\Juice\Juice.exe` so it can be tried.
3. Build the full release zip `Juice-x.y.z.zip`: one top folder `Juice/` with `Juice.exe` and `README.txt`. Save it to `C:\Claude Projects\Releases\Juice-x.y.z\`. A zip with only the exe is not a release zip.
4. Push the branch. Push to `main` when the owner says push or release. Never force-push. Tags are plain versions (`vX.Y.Z`) and are created on GitHub when the owner publishes.
5. Do not create the GitHub release or tag yourself. The owner attaches the zip and clicks Publish. The "My project apps" chat prepares the release page and checks it afterwards; it does not rebuild.
6. Finish with one message: what changed in short plain bullets (these become the release notes, no dashes), the version, the exe's SHA256, the zip path, the tests run, and what only real Windows can check.

## Working rules (all Pingo apps)
- Apps: Echoa, Fizz, Juice, Pour, Pulp, Minibar. Minibar is the hub that finds, installs, starts, stops and updates the others from GitHub releases (iiiPingo/<app>). Don't change how an app is named, found, installed, updated or closed without checking it still works with Minibar.
- Never rely on the exe's file name. Discord and browsers rename downloads to `Juice (2).exe`. Identify an app by its version resource (ProductName), not by `<App>.exe`.
- Every release needs a zip with the app name in its file name, containing `<App>.exe`. Bump the exe's version every release.
- Never touch user data (settings, logs, history, databases).
- This repo is public: never commit secrets, tokens, personal paths, build output (`out/`), logs or settings files.
- Work on a branch. Don't push, open a PR or publish a release until the owner says so. "Push", "release" and "update GitHub" count as that go-ahead: then push to `main`.
- Add a test for each fix that can be tested off Windows. At the end, list what can only be checked on real Windows or real hardware.
- If unsure a fix is safe, report it instead of changing it.
- Keep README text in plain words; mention what's new each version.
- Explain changes simply, in plain language.
