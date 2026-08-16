# Redshift

Redshift is a very narrow-scope sandbox that filters Discord's view of process
names, such as those used for game detection.

Redshift is a native x64 Windows 11 application. It does not install a driver,
service, or global hook, and it does not modify Discord on disk. It runs without
administrator privileges. Discord screen sharing and application capture are
designed to continue working normally.

The Discord desktop client can enumerate other programs on the PC. Discord's activity
privacy settings control whether that activity is shown to others, but they do not
offer a way to stop the client from reading process names locally. Redshift
exists only in lieu of that option, on the machine you already control. The
same kind of restriction is often applied by running the desktop client in a sandbox
such as Sandboxie, which also loads a DLL into the client to restrict what it
can see. Redshift applies that sandbox approach only selectively to process-name enumeration.
It is not a Discord product and has no affiliation.

## Liability

Redshift is provided as-is, without warranty. Use it at your own risk. The authors
are not responsible for ToS/account issues, data loss, service disruption, security
software actions, or any other damage resulting from its use or inability to work.

## Usage

This repository ships source only. Build it (see **Build**) and keep these
outputs together:

- `Redshift.exe`
- `RedshiftPrivacyHook.dll`

Quit Discord completely before launching it through Redshift. An already existing
Discord instance cannot be protected.

1. Run `Redshift.exe`.
2. Select the installed `Discord.exe` if it was not detected automatically.
3. Click **Launch protected Discord**.

Discord installs updates into versioned `app-*` directories. A saved path is
resolved to the newest installed `Discord.exe` before each launch. The selected
path is stored locally in `%LOCALAPPDATA%\Redshift\settings.ini`.

### Desktop shortcut

Right-click `Redshift.exe` and select **Show more options > Send to > Desktop
(create shortcut)**. For one-click protected launch, append `--launch` and the
Discord path shown in Redshift to the shortcut's **Target** like in the example below:

```text
"C:\Tools\Redshift\Redshift.exe" --launch "C:\Users\you\AppData\Local\Discord\app-1.0.0000\Discord.exe"
```

The shortcut keeps this manual path unchanged so will break on update. Launching Redshift resolves it internally
to the newest installed Discord version each time it launches. Update the
shortcut only if `Redshift.exe` is moved or Discord version changes.

Explicit launch errors are appended to `%LOCALAPPDATA%\Redshift\launch.log`.
Redshift does not transmit any data, it runs entirely locally.

## How it works

`RedshiftPrivacyHook.dll` uses Microsoft Detours to filter process enumeration
inside Discord. It covers:

- `NtQuerySystemInformation(SystemProcessInformation)`
- `Process32FirstW` and `Process32NextW`

The DLL also intercepts `CreateProcessW` so Discord and its updater pass the same
filter to later Discord child processes. Other child applications are started
normally.

## Security


Endpoint protection may flag DLL injection even when the software is legitimate.
Do not disable or bypass security software or settings to run Redshift.

A local Release build will not be Authenticode-signed. Windows SmartScreen may show
**Windows protected your PC** even if you built it from source.

Windows 11 **Smart App Control** may block unsigned software and does not offer
per-app exceptions. Self-sign only if you trust this source.

Redshift is not a security boundary. Discord retains its normal access to files,
devices, the network, windows, and other user resources, and it can detect the
loaded DLL.

The hook is local to Discord instances started by Redshift. It does not hide
windows, files, services, drivers or network activity.

## Build

Microsoft Detours 4.0.1 is vendored under `third_party/detours` under the MIT
license. A Release build produces `Redshift.exe` and
`RedshiftPrivacyHook.dll`.
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

### Ninja

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Visual Studio

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
