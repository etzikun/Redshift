#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <detours.h>
#include <cstdint>
#include <limits>
#include <cwchar>

namespace {
using QueryFn = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
QueryFn g_query{};
using CreateProcessWFn = BOOL (WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
    LPPROCESS_INFORMATION);
CreateProcessWFn g_createProcess = CreateProcessW;
using Process32Fn = BOOL (WINAPI*)(HANDLE, LPPROCESSENTRY32W);
Process32Fn g_processFirst{};
Process32Fn g_processNext{};
char g_hookPath[MAX_PATH]{};
constexpr wchar_t kLaunchStatusVariable[] = L"REDSHIFT_LAUNCH_STATUS";

bool SignalStatus(const wchar_t* suffix) {
    wchar_t base[96]{};
    const DWORD length = GetEnvironmentVariableW(kLaunchStatusVariable, base, ARRAYSIZE(base));
    if (!length || length >= ARRAYSIZE(base)) return false;
    wchar_t name[112]{};
    if (wcscpy_s(name, base) || wcscat_s(name, suffix)) return false;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!event) return false;
    const bool signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    return signaled;
}

bool StatusRequested() {
    wchar_t value[2]{};
    const DWORD length = GetEnvironmentVariableW(kLaunchStatusVariable, value, ARRAYSIZE(value));
    return length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

const wchar_t* BaseName(LPCWSTR path) {
    if (!path || !*path) return L"";
    const wchar_t* name = path;
    for (const wchar_t* cursor = path; *cursor; ++cursor)
        if (*cursor == L'\\' || *cursor == L'/') name = cursor + 1;
    return name;
}

bool ParentNamed(LPCWSTR path, const wchar_t* expected) {
    const wchar_t* name = BaseName(path);
    if (!*name || name == path) return false;
    const wchar_t* end = name;
    while (end > path && (end[-1] == L'\\' || end[-1] == L'/')) --end;
    const wchar_t* start = path;
    for (const wchar_t* cursor = path; cursor < end; ++cursor)
        if (*cursor == L'\\' || *cursor == L'/') start = cursor + 1;
    const size_t length = static_cast<size_t>(end - start);
    return length == wcslen(expected) && _wcsnicmp(start, expected, length) == 0;
}

bool ShouldInjectPath(LPCWSTR path) {
    if (_wcsicmp(BaseName(path), L"Discord.exe") == 0) return true;
    return _wcsicmp(BaseName(path), L"Update.exe") == 0 &&
        (ParentNamed(path, L"Discord") || ParentNamed(path, L"DiscordPTB") ||
         ParentNamed(path, L"DiscordCanary"));
}

bool ShouldInject(LPCWSTR application, LPCWSTR command) {
    if (ShouldInjectPath(application)) return true;
    if (!command) return false;
    while (*command == L' ' || *command == L'\t') ++command;
    bool quoted = *command == L'\"';
    if (quoted) ++command;
    wchar_t executable[MAX_PATH]{};
    size_t count = 0;
    while (*command && count + 1 < ARRAYSIZE(executable) &&
           ((quoted && *command != L'\"') || (!quoted && *command != L' ' && *command != L'\t')))
        executable[count++] = *command++;
    return ShouldInjectPath(executable);
}

bool KeepImage(const wchar_t* name) {
    if (!name || !*name) return true;
    return _wcsicmp(name, L"Discord.exe") == 0;
}

BOOL WINAPI PrivacyCreateProcessW(LPCWSTR application, LPWSTR command,
    LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes,
    BOOL inheritHandles, DWORD flags, LPVOID environment, LPCWSTR currentDirectory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    if (!ShouldInject(application, command) || !g_hookPath[0])
        return g_createProcess(application, command, processAttributes, threadAttributes,
            inheritHandles, flags, environment, currentDirectory, startup, process);
    return DetourCreateProcessWithDllExW(application, command, processAttributes,
        threadAttributes, inheritHandles, flags, environment, currentDirectory, startup,
        process, g_hookPath, g_createProcess);
}

bool Keep(const SYSTEM_PROCESS_INFORMATION* item) {
    if (!item->ImageName.Buffer || !item->ImageName.Length) return true;
    constexpr wchar_t discord[] = L"Discord.exe";
    return item->ImageName.Length == (ARRAYSIZE(discord) - 1) * sizeof(wchar_t) &&
        _wcsnicmp(item->ImageName.Buffer, discord, ARRAYSIZE(discord) - 1) == 0;
}

bool ValidateProcessList(const void* buffer, size_t length) {
    if (!buffer || length < sizeof(SYSTEM_PROCESS_INFORMATION)) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(buffer);
    if (length > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    const auto end = begin + length;
    auto current = begin;
    for (;;) {
        if (current > end - sizeof(SYSTEM_PROCESS_INFORMATION)) return false;
        const auto* item = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(current);
        if (item->ImageName.Length) {
            if (!item->ImageName.Buffer || item->ImageName.Length % sizeof(wchar_t) != 0 ||
                item->ImageName.MaximumLength < item->ImageName.Length) return false;
            const auto image = reinterpret_cast<std::uintptr_t>(item->ImageName.Buffer);
            if (image < begin || image > end || item->ImageName.Length > end - image) return false;
        }
        if (!item->NextEntryOffset) return true;
        if (item->NextEntryOffset < sizeof(SYSTEM_PROCESS_INFORMATION) ||
            item->NextEntryOffset > end - current - sizeof(SYSTEM_PROCESS_INFORMATION)) return false;
        current += item->NextEntryOffset;
    }
}

NTSTATUS NTAPI PrivacyQuery(SYSTEM_INFORMATION_CLASS type, PVOID buffer,
                            ULONG length, PULONG returned) {
    const NTSTATUS status = g_query(type, buffer, length, returned);
    if (status < 0 || type != SystemProcessInformation || !buffer) return status;
    if (returned && *returned > length) return status;
    const size_t valid = returned ? *returned : length;
    if (!ValidateProcessList(buffer, valid)) return status;
    auto* current = static_cast<SYSTEM_PROCESS_INFORMATION*>(buffer);
    if (!Keep(current)) return status;
    while (current->NextEntryOffset) {
        BYTE* const currentBytes = reinterpret_cast<BYTE*>(current);
        auto* next = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(currentBytes + current->NextEntryOffset);
        if (Keep(next)) {
            current = next;
        } else if (!next->NextEntryOffset) {
            current->NextEntryOffset = 0;
        } else {
            current->NextEntryOffset += next->NextEntryOffset;
        }
    }
    return status;
}

BOOL WINAPI PrivacyProcessNext(HANDLE snapshot, LPPROCESSENTRY32W entry) {
    if (!entry) return FALSE;
    while (g_processNext(snapshot, entry)) {
        if (KeepImage(entry->szExeFile)) return TRUE;
    }
    return FALSE;
}

BOOL WINAPI PrivacyProcessFirst(HANDLE snapshot, LPPROCESSENTRY32W entry) {
    if (!entry || !g_processFirst(snapshot, entry)) return FALSE;
    return KeepImage(entry->szExeFile) ? TRUE : PrivacyProcessNext(snapshot, entry);
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
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        g_query = ntdll ? reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQuerySystemInformation")) : nullptr;
        g_processFirst = kernel ? reinterpret_cast<Process32Fn>(GetProcAddress(kernel, "Process32FirstW")) : nullptr;
        g_processNext = kernel ? reinterpret_cast<Process32Fn>(GetProcAddress(kernel, "Process32NextW")) : nullptr;
        const bool statusRequested = StatusRequested();
        LONG result = g_query && g_processFirst && g_processNext
            ? DetourTransactionBegin() : ERROR_PROC_NOT_FOUND;
        const bool transactionStarted = result == NO_ERROR;
        if (result == NO_ERROR) result = DetourUpdateThread(GetCurrentThread());
        if (result == NO_ERROR)
            result = DetourAttach(reinterpret_cast<PVOID*>(&g_query), reinterpret_cast<PVOID>(PrivacyQuery));
        if (result == NO_ERROR)
            result = DetourAttach(reinterpret_cast<PVOID*>(&g_createProcess), reinterpret_cast<PVOID>(PrivacyCreateProcessW));
        if (result == NO_ERROR)
            result = DetourAttach(reinterpret_cast<PVOID*>(&g_processFirst), reinterpret_cast<PVOID>(PrivacyProcessFirst));
        if (result == NO_ERROR)
            result = DetourAttach(reinterpret_cast<PVOID*>(&g_processNext), reinterpret_cast<PVOID>(PrivacyProcessNext));
        if (result == NO_ERROR) {
            result = DetourTransactionCommit();
        } else if (transactionStarted) {
            DetourTransactionAbort();
        }
        if (result != NO_ERROR || (statusRequested && !SignalStatus(L".Ready"))) {
            if (statusRequested) SignalStatus(L".Failed");
            return FALSE;
        }
        if (statusRequested) SetEnvironmentVariableW(kLaunchStatusVariable, nullptr);
    } else if (reason == DLL_PROCESS_DETACH && g_query && DetourTransactionBegin() == NO_ERROR) {
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&g_query), reinterpret_cast<PVOID>(PrivacyQuery));
        DetourDetach(reinterpret_cast<PVOID*>(&g_createProcess), reinterpret_cast<PVOID>(PrivacyCreateProcessW));
        if (g_processFirst)
            DetourDetach(reinterpret_cast<PVOID*>(&g_processFirst), reinterpret_cast<PVOID>(PrivacyProcessFirst));
        if (g_processNext)
            DetourDetach(reinterpret_cast<PVOID*>(&g_processNext), reinterpret_cast<PVOID>(PrivacyProcessNext));
        DetourTransactionCommit();
    }
    return TRUE;
}
