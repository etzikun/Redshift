# Changelog

## Unreleased

### Changed

- Replaced automatic Windows theme detection with a manual Light/Dark theme
  selection saved in Redshift's local settings.
- Made the Discord path optional for `--launch`. Redshift now retrieves the
  saved path through `discord_install` and resolves the newest installed
  Discord version automatically.
- Centralized process visibility and injection decisions in a shared privacy
  policy with an explicit visible-process list, and separated process-list
  validation from filtering.
- Rewrote SYSTEM_PROCESS_INFORMATION filtering for readability: validation
  walks the buffer, then a separate pass unlinks hidden entries and leaves
  the list head in place.
- Narrowed DLL propagation to paths under the verified
  `%LOCALAPPDATA%\Discord` root (`Update.exe` and `app-X.Y.Z\Discord.exe`)
  instead of matching those basenames anywhere.
- Documented an explicit contract at the top of the hook listing the complete
  intercepted-API surface (`NtQuerySystemInformation` process lists,
  `Process32FirstW`/`Process32NextW`, and `CreateProcessW`).
- Made all four Detours attach/detach calls explicit and switched available
  Windows APIs, including `NtQuerySystemInformation`, to normal imports.
- Removed undocumented ordinal-based dark-mode API resolution, the **Open log**
  shell action, and stale `wintrust`/`crypt32` linker entries from SHA-256 verification.
- Updated the application icon.
