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

inline std::wstring NormalizedPath(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::wstring full(32768, L'\0');
    const DWORD fullLength = GetFullPathNameW(
        path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!fullLength || fullLength >= full.size()) return {};
    std::wstring longer(32768, L'\0');
    const DWORD longLength = GetLongPathNameW(
        full.c_str(), longer.data(), static_cast<DWORD>(longer.size()));
    std::wstring result = longLength && longLength < longer.size()
        ? std::wstring(longer.data(), longLength) : std::wstring(full.data(), fullLength);
    if (result.size() >= 7 && result[0] == L'\\' && result[1] == L'\\' &&
        result[2] == L'?' && result[3] == L'\\' && result[5] == L':' && result[6] == L'\\')
        result.erase(0, 4);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

inline bool IsUnderDirectory(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() <= root.size()) return false;
    if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) return false;
    return path[root.size()] == L'\\' || path[root.size()] == L'/';
}

inline std::filesystem::path DefaultDiscordRoot() {
    std::vector<wchar_t> local(32768);
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (length == 0 || length >= local.size()) return {};
    return std::filesystem::path(local.data()) / L"Discord";
}

inline std::filesystem::path DiscordInstallRoot(const std::filesystem::path& exe) {
    const auto normalized = NormalizedPath(exe);
    if (normalized.empty() || !privacy_policy::IsDiscordExecutableName(normalized)) return {};
    const auto parent = std::filesystem::path(normalized).parent_path();
    int parts[3]{};
    if (!ParseAppFolder(parent.filename().wstring(), parts)) return {};
    return parent.parent_path();
}

inline bool IsStandardInstallRoot(const std::filesystem::path& root) {
    if (root.empty()) return false;
    std::vector<wchar_t> local(32768);
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (length == 0 || length >= local.size()) return false;
    if (privacy_policy::IsVerifiedDiscordInstallationRoot(root.native(), local.data())) return true;
    const auto normalizedRoot = NormalizedPath(root);
    const auto normalizedLocal = NormalizedPath(local.data());
    return !normalizedRoot.empty() && !normalizedLocal.empty() &&
        privacy_policy::IsVerifiedDiscordInstallationRoot(normalizedRoot, normalizedLocal);
}

inline bool IsInsideStandardInstall(const std::filesystem::path& path) {
    const auto root = DefaultDiscordRoot();
    if (root.empty() || path.empty()) return false;
    if (IsUnderDirectory(path.wstring(), root.wstring())) return true;
    return IsUnderDirectory(NormalizedPath(path), NormalizedPath(root));
}

// A path selects an install only when it lives outside this machine's standard
// Discord directory. Version folders and paths from another machine do not.
inline std::filesystem::path ResolveDiscord(const std::filesystem::path& selected) {
    const auto standardRoot = DefaultDiscordRoot();
    const auto selectedRoot = DiscordInstallRoot(selected);
    if (!selectedRoot.empty() && !IsStandardInstallRoot(selectedRoot)) {
        if (const auto latest = LatestDiscord(selectedRoot); !latest.empty()) return latest;
    }
    std::error_code error;
    const auto normalized = NormalizedPath(selected);
    if (!normalized.empty() && !IsInsideStandardInstall(normalized) &&
        std::filesystem::is_regular_file(normalized, error) &&
        privacy_policy::IsDiscordExecutableName(normalized))
        return normalized;
    return LatestDiscord(standardRoot);
}

inline std::wstring ManualDiscordSetting(const std::filesystem::path& shown) {
    const auto resolved = ResolveDiscord(shown);
    if (resolved.empty() || IsInsideStandardInstall(resolved)) return {};
    return resolved.wstring();
}
