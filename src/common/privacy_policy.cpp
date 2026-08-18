#include "privacy_policy.h"

#include <cwchar>

namespace privacy_policy {
namespace {

constexpr std::wstring_view kVisibleProcessImages[] = {
    L"Discord.exe",
};

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
        _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

std::wstring_view ExecutableImageName(std::wstring_view path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

bool IsPathSeparator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

std::wstring_view WithoutTrailingSeparators(std::wstring_view path) {
    while (!path.empty() && IsPathSeparator(path.back())) path.remove_suffix(1);
    return path;
}

bool IsThreePartAppDirectory(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"app-";
    if (name.size() <= prefix.size() ||
        !EqualsIgnoreCase(name.substr(0, prefix.size()), prefix)) return false;

    size_t separators = 0;
    bool partHasDigit = false;
    for (const wchar_t character : name.substr(prefix.size())) {
        if (character == L'.') {
            if (!partHasDigit || separators >= 2) return false;
            ++separators;
            partHasDigit = false;
        } else if (character < L'0' || character > L'9') {
            return false;
        } else {
            partHasDigit = true;
        }
    }
    return separators == 2 && partHasDigit;
}

std::wstring_view PathBelowRoot(std::wstring_view path, std::wstring_view root) {
    root = WithoutTrailingSeparators(root);
    if (path.size() <= root.size() ||
        !EqualsIgnoreCase(path.substr(0, root.size()), root) ||
        !IsPathSeparator(path[root.size()])) return {};
    return path.substr(root.size() + 1);
}

}

bool KeepProcessImage(std::wstring_view image) {
    if (image.empty()) return true;
    for (const auto visible : kVisibleProcessImages)
        if (EqualsIgnoreCase(image, visible)) return true;
    return false;
}

bool IsDiscordExecutableName(std::wstring_view executablePath) {
    return EqualsIgnoreCase(ExecutableImageName(executablePath), L"Discord.exe");
}

bool IsVerifiedDiscordInstallationRoot(std::wstring_view installationRoot,
                                       std::wstring_view localAppData) {
    installationRoot = WithoutTrailingSeparators(installationRoot);
    localAppData = WithoutTrailingSeparators(localAppData);
    if (localAppData.empty() || installationRoot.size() != localAppData.size() + 8)
        return false;
    if (!EqualsIgnoreCase(installationRoot.substr(0, localAppData.size()), localAppData) ||
        !IsPathSeparator(installationRoot[localAppData.size()])) return false;
    return EqualsIgnoreCase(installationRoot.substr(localAppData.size() + 1), L"Discord");
}

bool ShouldInjectInto(std::wstring_view executablePath,
                      std::wstring_view verifiedInstallationRoot) {
    // Strict propagation contract:
    //   %LOCALAPPDATA%\Discord\Update.exe
    //   %LOCALAPPDATA%\Discord\app-X.Y.Z\Discord.exe
    const std::wstring_view relative =
        PathBelowRoot(executablePath, verifiedInstallationRoot);
    if (relative.empty()) return false;
    if (EqualsIgnoreCase(relative, L"Update.exe")) return true;

    const size_t separator = relative.find_first_of(L"\\/");
    if (separator == std::wstring_view::npos) return false;
    if (relative.find_first_of(L"\\/", separator + 1) != std::wstring_view::npos)
        return false;
    return IsThreePartAppDirectory(relative.substr(0, separator)) &&
        EqualsIgnoreCase(relative.substr(separator + 1), L"Discord.exe");
}

}
