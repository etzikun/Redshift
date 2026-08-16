# Redshift
Redshift is a very narrow-scope sandbox that filters Discord's view of process
names, such as those used for game detection.

Redshift is a native x64 Windows 11 application. It does not install a driver,
service, global hook and it does not modify Discord on disk. It runs without
administrator privileges. Discord screen sharing and application capture are
designed to continue working normally.

The Discord desktop client can enumerate other programs on the PC. Discord's activity
privacy settings control whether that activity is shown to others but they do not
offer a way to stop the client from reading process names locally. Redshift
exists only in lieu of that option, on the machine you already control. The
same kind of limit is often applied by running the desktop client in a sandbox
such as Sandboxie, which also loads a DLL into the client to restrict what it
can see. Redshift is a very narrow-scope sandbox for that one case: it filters
Discord's view of process names only. It does not isolate files, devices, the
network, windows, or other system resources. It is not a Discord product. This
is the project's sole purpose.

## Liability

Redshift is provided as-is, without warranty. Use it at your own risk. The authors
are not responsible for ToS/account issues, data loss, service disruption, security
software actions, or other damage resulting from its use or inability to work.

## Usage

Keep these files together:

- `Redshift.exe`
- `RedshiftPrivacyHook.dll`

Quit Discord completely before launching it through Redshift. An existing
Discord instance cannot be retroactively protected.

1. Run `Redshift.exe`.
2. Select the installed `Discord.exe` if it was not detected automatically.
3. Click **Launch protected Discord**.

Discord installs updates into versioned `app-*` directories. A saved path is
resolved to the newest installed `Discord.exe` before each launch. The selected
path is stored locally in `%LOCALAPPDATA%\Redshift\settings.ini`.

**Create desktop shortcut** writes `Discord (Redshift).lnk` to the desktop. The
shortcut starts Redshift in headless mode, launches protected Discord, then
exits cleanly. Recreate the shortcut if `Redshift.exe` is moved.

Explicit launch errors are appended to `%LOCALAPPDATA%\Redshift\launch.log`. 
Redshift does not transmit the settings or log data.

## How it works

`RedshiftPrivacyHook.dll` uses Microsoft Detours to filter process enumeration
inside Discord. It covers:

- `NtQuerySystemInformation(SystemProcessInformation)`
- `Process32FirstW` and `Process32NextW`

The DLL also intercepts `CreateProcessW` so Discord and its updater pass the same
filter to later Discord child processes. Other child applications are started
normally.

## Security

This is a very narrow-scope sandbox: process names only. Discord retains its
normal access to files, devices, the network, windows, and other user resources.
The Discord client can also detect the loaded DLL (as such, use at your own risk).

The hook is local to Discord instances started by Redshift. It does not hide
windows, files, services, drivers or network activity.

Endpoint protection may flag DLL injection even when the software is legitimate.
Do not disable or bypass security software to run Redshift.

## Build

Microsoft Detours 4.0.1 is vendored under `third_party/detours` under the MIT
license. A Release build produces `Redshift.exe` and
`RedshiftPrivacyHook.dll`

Build requirements:
- Windows 11, x64
- Visual Studio 2022 or Visual Studio Build Tools with:
  - Desktop development with C++
  - MSVC C++ toolchain
  - Windows SDK
- CMake 3.20 or newer
- Ninja, only when using the Ninja commands below

Run the commands below from the **x64 Native Tools Command Prompt for Visual
Studio**. A 32-bit configuration is rejected because it cannot inject into
64-bit Discord.

Ninja (powershell)
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Visual Studio (powershell)
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
