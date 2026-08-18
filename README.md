# Redshift

Redshift is a very narrow-scope sandbox that filters Discord's view of process
names, such as those used for game detection.

Redshift is a native x64 Windows 11 application. It does not install a driver,
service, or global hook, and it does not modify Discord on disk. It runs without
administrator privileges. Discord screen sharing and application capture are
designed to continue working normally.

The Discord desktop client can enumerate other programs on the PC. Discord's
activity privacy settings control whether that activity is shown to others,
but they do not stop the client from reading process names locally. Redshift is
for users who do not want the desktop client to receive unrelated process names
at all, regardless of how Discord subsequently uses them. The same
kind of restriction is often applied by running the desktop client in a sandbox
such as Sandboxie, which also loads a DLL into the client to restrict what it
can see. Redshift applies that approach only to process-name enumeration.
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

Redshift supports the standard Discord installation at
`%LOCALAPPDATA%\Discord`. The selected executable must be
`app-X.Y.Z\Discord.exe` under that directory.

1. Run `Redshift.exe`.
2. Select the installed `Discord.exe` if it was not detected automatically.
3. Click **Launch protected Discord**.

Discord installs updates into versioned `app-*` directories. A saved path is
resolved to the newest installed `Discord.exe` before each launch. The selected
path and the Light/Dark theme choice are stored locally in
`%LOCALAPPDATA%\Redshift\settings.ini`.

Explicit launch errors are appended to `%LOCALAPPDATA%\Redshift\launch.log`.
Successful launches are not logged. Redshift runs entirely locally and does not
transmit data.

### Desktop shortcut

Right-click `Redshift.exe` and select **Show more options > Send to > Desktop
(create shortcut)**. Open the shortcut's **Properties** and append `--launch`
to **Target**:

```text
"C:\Tools\Redshift\Redshift.exe" --launch
```

This quick-start mode uses the saved Discord location, resolves the newest
installed version automatically, and exits after a verified launch. Redshift
does not show a window on success. If the launch fails, its normal window opens
and displays the error.

If no location has been saved, Redshift checks Discord's normal install
location. The older `--launch "path\to\Discord.exe"` form is also supported.
The shortcut only needs updating if `Redshift.exe` is moved.

## How it works

`RedshiftPrivacyHook.dll` uses Microsoft Detours to filter process enumeration
inside Discord. Process visibility is controlled by a small explicit allow-list.
The complete intercepted API surface is:

- `NtQuerySystemInformation(SystemProcessInformation)`
- `Process32FirstW` and `Process32NextW`
- `CreateProcessW`

`CreateProcessW` propagation is restricted to
`%LOCALAPPDATA%\Discord\Update.exe` and
`%LOCALAPPDATA%\Discord\app-X.Y.Z\Discord.exe`. Other child applications are
started normally.

The native `NtQuerySystemInformation` function is linked normally through the
Windows SDK's `ntdll.lib`.

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

### Advisory static analysis

Redshift is periodically scanned with pinned versions of the Semgrep engine and
[Semgrep Community Edition rules](https://github.com/semgrep/semgrep-rules).

These scans are informational only. They are not a security certification
and are not used as a release gate.

Engine and rules updates are made through reviewed pull requests. Each scan
records the exact Semgrep rules commit, Redshift commit, and engine version used.

Findings are published for awareness and independent review. A clean scan
should not be interpreted as proof that Redshift is safe.

## Build

Microsoft Detours 4.0.1 is vendored under `third_party/detours` under the MIT
license; its retained-file hashes are recorded in
`third_party/detours/PROVENANCE.md`. A Release build produces `Redshift.exe` and
`RedshiftPrivacyHook.dll`. The hook also links the Windows SDK import library
`ntdll.lib` for `NtQuerySystemInformation`; Windows supplies `ntdll.dll`.
CI checks the vendored file set and hashes against that provenance record.
Build requirements:

- Windows 11, x64
- Visual Studio 2022 or newer, or Visual Studio Build Tools, with:
  - Desktop development with C++
  - MSVC C++20 toolchain
  - Windows 11 SDK
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
