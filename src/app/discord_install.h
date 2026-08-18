#pragma once

#include "privacy_policy.h"
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

inline bool ParseAppFolder(const std::wstring& name, int parts[3]) {
    if (name.size() < 5 || _wcsnicmp(name.c_str(), L"app-", 4) != 0) return false;
    const wchar_t* cursor = name.c_str() + 4;
    for (int i = 0; i < 3; ++i) {
        if (*cursor < L'0' || *cursor > L'9') return false;
        wchar_t* end{};
        const long value = wcstol(cursor, &end, 10);
        if (end == cursor || value < 0 || value > 1000000) return false;
        parts[i] = static_cast<int>(value);
        cursor = end;
        if (i < 2) {
            if (*cursor != L'.') return false;
            ++cursor;
        }
    }
    return *cursor == 0;
}

inline int CompareAppFolder(const std::wstring& left, const std::wstring& right) {
    int a[3]{}, b[3]{};
    if (ParseAppFolder(left, a) && ParseAppFolder(right, b)) {
        for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    }
    return _wcsicmp(left.c_str(), right.c_str());
}

inline std::filesystem::path LatestDiscord(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::path latest;
    std::wstring latestName;
    if (!std::filesystem::exists(root, error)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (!entry.is_directory(error)) continue;
        const auto name = entry.path().filename().wstring();
        int parts[3]{};
        if (!ParseAppFolder(name, parts)) continue;
        auto candidate = entry.path() / L"Discord.exe";
        if (!std::filesystem::is_regular_file(candidate, error)) continue;
        if (latest.empty() || CompareAppFolder(name, latestName) > 0) {
            latest = candidate;
            latestName = name;
        }
    }
    return latest;
}

inline std::filesystem::path DiscordInstallRoot(const std::filesystem::path& exe) {
    if (!privacy_policy::IsDiscordExecutableName(exe.native())) return {};
    int parts[3]{};
    if (!ParseAppFolder(exe.parent_path().filename().wstring(), parts)) return {};
    return exe.parent_path().parent_path();
}

inline std::filesystem::path DefaultDiscordRoot() {
    std::vector<wchar_t> local(32768);
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (length == 0 || length >= local.size()) return {};
    return std::filesystem::path(local.data()) / L"Discord";
}

inline std::filesystem::path ResolveDiscord(const std::filesystem::path& selected) {
    std::error_code error;
    if (const auto root = DiscordInstallRoot(selected); !root.empty()) {
        if (const auto latest = LatestDiscord(root); !latest.empty()) return latest;
    }
    if (std::filesystem::is_regular_file(selected, error) &&
        privacy_policy::IsDiscordExecutableName(selected.native())) return selected;
    return LatestDiscord(DefaultDiscordRoot());
}
