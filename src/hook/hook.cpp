#include "privacy_policy.h"
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <detours.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

// This DLL intercepts only:
// - NtQuerySystemInformation(SystemProcessInformation)
// - Process32FirstW
// - Process32NextW
// - CreateProcessW
// for the purposes of filtering process enumeration and propagating the launch hook to child processes, as outlined in the README.
// It does not intercept any other APIs.

namespace {
using QueryFn = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
QueryFn g_ntQuerySystemInformation = NtQuerySystemInformation;
using CreateProcessWFn = BOOL (WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
    LPPROCESS_INFORMATION);
CreateProcessWFn g_createProcessW = CreateProcessW;
using Process32Fn = BOOL (WINAPI*)(HANDLE, LPPROCESSENTRY32W);
Process32Fn g_process32FirstW = Process32FirstW;
Process32Fn g_process32NextW = Process32NextW;
char g_hookPath[MAX_PATH]{};
constexpr wchar_t kDiscordRootVariable[] = L"REDSHIFT_DISCORD_ROOT";
constexpr wchar_t kLaunchStatusVariable[] = L"REDSHIFT_LAUNCH_STATUS";
wchar_t g_verifiedDiscordRoot[MAX_PATH]{};
bool g_privacyHooksInstalled{};

bool SignalLaunchHookStatus(const wchar_t* suffix) {
    wchar_t base[96]{};
    const DWORD length = GetEnvironmentVariableW(kLaunchStatusVariable, base, ARRAYSIZE(base));
    if (!length || length >= ARRAYSIZE(base)) return false;
    wchar_t name[112]{};
    if (wcscpy_s(name, base) || wcscat_s(name, suffix)) return false;
    // SIDE EFFECT: signal the named launch-verification event.
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!event) return false;
    const bool signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    return signaled;
}

bool IsLaunchHookStatusRequested() {
    wchar_t value[2]{};
    const DWORD length = GetEnvironmentVariableW(kLaunchStatusVariable, value, ARRAYSIZE(value));
    return length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

bool LoadVerifiedDiscordInstallationRoot() {
    wchar_t root[MAX_PATH]{};
    const DWORD rootLength =
        GetEnvironmentVariableW(kDiscordRootVariable, root, ARRAYSIZE(root));
    if (!rootLength || rootLength >= ARRAYSIZE(root)) return false;

    wchar_t localAppData[MAX_PATH]{};
    const DWORD localLength =
        GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (!localLength || localLength >= ARRAYSIZE(localAppData) ||
        !privacy_policy::IsVerifiedDiscordInstallationRoot(root, localAppData)) return false;
    return wcscpy_s(g_verifiedDiscordRoot, root) == 0;
}

bool ShouldInjectIntoCreatedProcess(LPCWSTR application, LPCWSTR command) {
    if (application &&
        privacy_policy::ShouldInjectInto(application, g_verifiedDiscordRoot)) return true;
    if (!command) return false;
    while (*command == L' ' || *command == L'\t') ++command;
    bool quoted = *command == L'\"';
    if (quoted) ++command;
    wchar_t executable[MAX_PATH]{};
    size_t count = 0;
    while (*command && count + 1 < ARRAYSIZE(executable) &&
           ((quoted && *command != L'\"') || (!quoted && *command != L' ' && *command != L'\t')))
        executable[count++] = *command++;
    return privacy_policy::ShouldInjectInto(executable, g_verifiedDiscordRoot);
}

BOOL WINAPI CreateProcessWWithHookPropagation(LPCWSTR application, LPWSTR command,
    LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes,
    BOOL inheritHandles, DWORD flags, LPVOID environment, LPCWSTR currentDirectory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    if (!ShouldInjectIntoCreatedProcess(application, command) || !g_hookPath[0])
        return g_createProcessW(application, command, processAttributes, threadAttributes,
            inheritHandles, flags, environment, currentDirectory, startup, process);
    // SIDE EFFECT: propagate RedshiftPrivacyHook.dll to an approved Discord child.
    return DetourCreateProcessWithDllExW(application, command, processAttributes,
        threadAttributes, inheritHandles, flags, environment, currentDirectory, startup,
        process, g_hookPath, g_createProcessW);
}

constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017L);

std::wstring_view SystemProcessImageName(const SYSTEM_PROCESS_INFORMATION& process) {
    if (!process.ImageName.Buffer || !process.ImageName.Length) return {};
    return {process.ImageName.Buffer, process.ImageName.Length / sizeof(wchar_t)};
}

SYSTEM_PROCESS_INFORMATION* SystemProcessNext(SYSTEM_PROCESS_INFORMATION* entry) {
    if (!entry->NextEntryOffset) return nullptr;
    return reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(
        reinterpret_cast<BYTE*>(entry) + entry->NextEntryOffset);
}

NTSTATUS QueryIntoPrivateBuffer(
    SYSTEM_INFORMATION_CLASS type, ULONG length, void** buffer, ULONG* written) {
    *buffer = nullptr;
    *written = 0;
    void* allocated = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length);
    if (!allocated) return kStatusNoMemory;

    ULONG returnedLength = 0;
    const NTSTATUS status =
        g_ntQuerySystemInformation(type, allocated, length, &returnedLength);
    *buffer = allocated;
    *written = returnedLength;
    return status;
}

void FreePrivateBuffer(void* buffer) {
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
}

bool ValidateSystemProcessInformation(const void* buffer, size_t bufferLength) {
    if (!buffer || bufferLength < sizeof(SYSTEM_PROCESS_INFORMATION)) return false;

    const auto bufferStart = reinterpret_cast<std::uintptr_t>(buffer);
    if (bufferLength > (std::numeric_limits<std::uintptr_t>::max)() - bufferStart)
        return false;
    const auto bufferEnd = bufferStart + bufferLength;
    auto entryAddress = bufferStart;

    for (;;) {
        const size_t bytesRemaining = static_cast<size_t>(bufferEnd - entryAddress);
        if (bytesRemaining < sizeof(SYSTEM_PROCESS_INFORMATION)) return false;

        const auto* entry =
            reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(entryAddress);
        const USHORT imageLength = entry->ImageName.Length;
        if (imageLength) {
            if (!entry->ImageName.Buffer || imageLength % sizeof(wchar_t) != 0 ||
                entry->ImageName.MaximumLength < imageLength) return false;

            const auto imageAddress =
                reinterpret_cast<std::uintptr_t>(entry->ImageName.Buffer);
            if (imageAddress < bufferStart || imageAddress > bufferEnd) return false;
            const size_t imageBytesAvailable =
                static_cast<size_t>(bufferEnd - imageAddress);
            if (imageLength > imageBytesAvailable) return false;
        }

        const ULONG nextOffset = entry->NextEntryOffset;
        if (!nextOffset) return true;

        // Each offset must advance to another complete entry within this buffer.
        if (nextOffset < sizeof(SYSTEM_PROCESS_INFORMATION)) return false;
        const size_t maximumNextOffset =
            bytesRemaining - sizeof(SYSTEM_PROCESS_INFORMATION);
        if (nextOffset > maximumNextOffset) return false;
        entryAddress += nextOffset;
    }
}

void ClearProcessImageName(SYSTEM_PROCESS_INFORMATION* entry) {
    if (entry->ImageName.Buffer && entry->ImageName.Length)
        SecureZeroMemory(entry->ImageName.Buffer, entry->ImageName.Length);
    entry->ImageName.Buffer = nullptr;
    entry->ImageName.Length = 0;
    entry->ImageName.MaximumLength = 0;
}

void FilterSystemProcessInformation(void* buffer) {
    auto* current = static_cast<SYSTEM_PROCESS_INFORMATION*>(buffer);

    // Keep the head in place because removing it would require compacting the
    // buffer and rebasing every embedded pointer. If policy ever rejects the
    // head, remove its name but retain its non-name process information.
    if (!privacy_policy::KeepProcessImage(SystemProcessImageName(*current)))
        ClearProcessImageName(current);

    while (current->NextEntryOffset) {
        const ULONG nextOffset = current->NextEntryOffset;
        auto* next = SystemProcessNext(current);

        if (privacy_policy::KeepProcessImage(SystemProcessImageName(*next))) {
            current = next;
            continue;
        }

        // Erase the hidden name before unlinking the entry so no process name
        // remains in the bytes that will be copied to the caller.
        ClearProcessImageName(next);
        const ULONG followingOffset = next->NextEntryOffset;
        if (!followingOffset) {
            current->NextEntryOffset = 0;
            return;
        }

        current->NextEntryOffset = nextOffset + followingOffset;
    }
}

void RebaseImageNamePointers(void* buffer, const void* oldBase, const void* newBase) {
    // ImageName.Buffer points into the query buffer, not a standalone
    // allocation. After the sanitized copy moves to Discord's address, those
    // pointers still refer to the temporary and must slide by (newBase - oldBase).
    if (!buffer || !oldBase || !newBase || oldBase == newBase) return;

    const auto oldStart = reinterpret_cast<std::uintptr_t>(oldBase);
    const auto newStart = reinterpret_cast<std::uintptr_t>(newBase);
    for (auto* entry = static_cast<SYSTEM_PROCESS_INFORMATION*>(buffer);
         entry; entry = SystemProcessNext(entry)) {
        if (!entry->ImageName.Buffer) continue;
        const auto image = reinterpret_cast<std::uintptr_t>(entry->ImageName.Buffer);
        if (image < oldStart) continue;
        entry->ImageName.Buffer = reinterpret_cast<PWSTR>(newStart + (image - oldStart));
    }
}

void CopySanitizedBufferToCaller(void* callerBuffer, size_t callerBufferLength,
                                 const void* sanitizedBuffer, size_t resultLength) {
    // NtQuerySystemInformation may return fewer bytes than the caller supplied.
    // Clear the complete caller buffer first so an earlier, larger result cannot
    // remain readable after the current result ends.
    SecureZeroMemory(callerBuffer, callerBufferLength);
    memcpy(callerBuffer, sanitizedBuffer, resultLength);
    RebaseImageNamePointers(callerBuffer, sanitizedBuffer, callerBuffer);
}

NTSTATUS NTAPI FilteredNtQuerySystemInformation(SYSTEM_INFORMATION_CLASS type, PVOID buffer,
                                                ULONG length, PULONG returned) {
    if (type != SystemProcessInformation || !buffer || !length)
        return g_ntQuerySystemInformation(type, buffer, length, returned);

    void* privateBuffer = nullptr;
    ULONG written = 0;
    const NTSTATUS status =
        QueryIntoPrivateBuffer(type, length, &privateBuffer, &written);
    if (!privateBuffer) {
        if (returned) *returned = written;
        return status;
    }

    if (status < 0 || written > length ||
        !ValidateSystemProcessInformation(privateBuffer, written)) {
        if (returned) *returned = status < 0 ? written : 0;
        FreePrivateBuffer(privateBuffer);
        return status < 0 ? status : kStatusUnsuccessful;
    }

    FilterSystemProcessInformation(privateBuffer);
    CopySanitizedBufferToCaller(buffer, length, privateBuffer, written);
    if (returned) *returned = written;
    FreePrivateBuffer(privateBuffer);
    return status;
}

BOOL WINAPI FilteredProcess32NextW(HANDLE snapshot, LPPROCESSENTRY32W output) {
    if (!output) return FALSE;

    const DWORD structureSize = output->dwSize;
    for (;;) {
        // Use a new zeroed structure for every underlying call. A skipped long
        // name therefore cannot remain in the unused tail of a later result.
        PROCESSENTRY32W candidate{};
        candidate.dwSize = structureSize;
        if (!g_process32NextW(snapshot, &candidate)) return FALSE;
        if (!privacy_policy::KeepProcessImage(candidate.szExeFile))
            continue;

        *output = candidate;
        return TRUE;
    }
}

BOOL WINAPI FilteredProcess32FirstW(HANDLE snapshot, LPPROCESSENTRY32W output) {
    if (!output) return FALSE;

    PROCESSENTRY32W candidate{};
    candidate.dwSize = output->dwSize;

    if (!g_process32FirstW(snapshot, &candidate)) return FALSE;
    if (privacy_policy::KeepProcessImage(candidate.szExeFile)) {
        *output = candidate;
        return TRUE;
    }
    return FilteredProcess32NextW(snapshot, output);
}

LONG InstallPrivacyHooks() {
    // SIDE EFFECT: install the complete four-API privacy hook surface.
    LONG result = DetourTransactionBegin();
    if (result != NO_ERROR) return result;

    result = DetourUpdateThread(GetCurrentThread());
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_ntQuerySystemInformation),
            reinterpret_cast<PVOID>(FilteredNtQuerySystemInformation));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_process32FirstW),
            reinterpret_cast<PVOID>(FilteredProcess32FirstW));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_process32NextW),
            reinterpret_cast<PVOID>(FilteredProcess32NextW));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_createProcessW),
            reinterpret_cast<PVOID>(CreateProcessWWithHookPropagation));

    if (result == NO_ERROR) return DetourTransactionCommit();
    DetourTransactionAbort();
    return result;
}

LONG RemovePrivacyHooks() {
    // SIDE EFFECT: remove the complete four-API privacy hook surface.
    LONG result = DetourTransactionBegin();
    if (result != NO_ERROR) return result;

    result = DetourUpdateThread(GetCurrentThread());
    if (result == NO_ERROR)
        result = DetourDetach(reinterpret_cast<PVOID*>(&g_ntQuerySystemInformation),
            reinterpret_cast<PVOID>(FilteredNtQuerySystemInformation));
    if (result == NO_ERROR)
        result = DetourDetach(reinterpret_cast<PVOID*>(&g_process32FirstW),
            reinterpret_cast<PVOID>(FilteredProcess32FirstW));
    if (result == NO_ERROR)
        result = DetourDetach(reinterpret_cast<PVOID*>(&g_process32NextW),
            reinterpret_cast<PVOID>(FilteredProcess32NextW));
    if (result == NO_ERROR)
        result = DetourDetach(reinterpret_cast<PVOID*>(&g_createProcessW),
            reinterpret_cast<PVOID>(CreateProcessWWithHookPropagation));

    if (result == NO_ERROR) return DetourTransactionCommit();
    DetourTransactionAbort();
    return result;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (DetourIsHelperProcess()) return TRUE;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        DetourRestoreAfterWith();
        wchar_t hookPath[MAX_PATH]{};
        if (GetModuleFileNameW(instance, hookPath, ARRAYSIZE(hookPath))) {
            BOOL substituted = FALSE;
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, hookPath, -1,
                g_hookPath, ARRAYSIZE(g_hookPath), nullptr, &substituted);
            if (substituted) g_hookPath[0] = '\0';
        }
        const bool statusRequested = IsLaunchHookStatusRequested();
        const bool hasVerifiedDiscordRoot = LoadVerifiedDiscordInstallationRoot();
        LONG result = hasVerifiedDiscordRoot ? InstallPrivacyHooks() : ERROR_INVALID_DATA;
        g_privacyHooksInstalled = result == NO_ERROR;
        if (result != NO_ERROR || (statusRequested && !SignalLaunchHookStatus(L".Ready"))) {
            if (statusRequested) SignalLaunchHookStatus(L".Failed");
            return FALSE;
        }
        // SIDE EFFECT: stop propagating the one-time launch-status variable.
        if (statusRequested) SetEnvironmentVariableW(kLaunchStatusVariable, nullptr);
    } else if (reason == DLL_PROCESS_DETACH && g_privacyHooksInstalled)
        RemovePrivacyHooks();
    return TRUE;
}
