#pragma once

#include <string_view>

namespace privacy_policy {

bool KeepProcessImage(std::wstring_view image);

// Standard installation root: exactly %LOCALAPPDATA%\Discord after trimming
// trailing separators. This is not a publisher or signature check.
bool IsVerifiedDiscordInstallationRoot(std::wstring_view installationRoot,
                                       std::wstring_view localAppData);

// Allows only <root>\Update.exe and <root>\app-X.Y.Z\Discord.exe.
// The root is this machine's standard install, or a manual install directory.
bool ShouldInjectInto(std::wstring_view executablePath,
                      std::wstring_view verifiedInstallationRoot);
bool IsDiscordExecutableName(std::wstring_view executablePath);

}
