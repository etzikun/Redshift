#pragma once

#include <string_view>

namespace privacy_policy {

bool KeepProcessImage(std::wstring_view image);

// "Verified installation root" means exactly %LOCALAPPDATA%\Discord after
// trimming trailing separators. It does not mean publisher or signature verification.
bool IsVerifiedDiscordInstallationRoot(std::wstring_view installationRoot,
                                       std::wstring_view localAppData);

// Allows only <root>\Update.exe and <root>\app-X.Y.Z\Discord.exe.
bool ShouldInjectInto(std::wstring_view executablePath,
                      std::wstring_view verifiedInstallationRoot);
bool IsDiscordExecutableName(std::wstring_view executablePath);

}
