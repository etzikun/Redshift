#include "resource.h"
#include "discord_install.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <detours.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <winreg.h>
#include <wintrust.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
constexpr wchar_t kClass[] = L"RedshiftWindow";
constexpr wchar_t kTitle[] = L"Redshift";
constexpr wchar_t kHookStatusVariable[] = L"REDSHIFT_HOOK_STATUS_EVENT";
enum { IdPath = 1001, IdBrowse, IdLaunch, IdShortcut, IdLog, IdStatus, IdHint };
HINSTANCE g_instance{};
HWND g_window{}, g_path{}, g_status{}, g_hint{};
HFONT g_font{}, g_titleFont{};

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

struct {
    int (WINAPI* setPreferredAppMode)(int);
    BOOL (WINAPI* allowDarkForWindow)(HWND, BOOL);
    void (WINAPI* flushMenuThemes)();
} g_dark{};

struct {
    bool dark{};
    COLORREF window{};
    COLORREF text{};
    COLORREF hint{};
    COLORREF edit{};
    HBRUSH windowBrush{};
    HBRUSH editBrush{};
} g_theme{};

bool HighContrastOn() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
}
bool AppsUseLightTheme() {
    DWORD value = 1, size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status != ERROR_SUCCESS || value != 0;
}
void DestroyThemeBrushes() {
    if (g_theme.windowBrush) DeleteObject(g_theme.windowBrush);
    if (g_theme.editBrush) DeleteObject(g_theme.editBrush);
    g_theme.windowBrush = g_theme.editBrush = nullptr;
}
void LoadTheme() {
    DestroyThemeBrushes();
    const bool highContrast = HighContrastOn();
    g_theme.dark = !highContrast && !AppsUseLightTheme();
    if (g_theme.dark) {
        g_theme.window = RGB(32, 32, 32);
        g_theme.text = RGB(250, 250, 250);
        g_theme.hint = RGB(166, 166, 166);
        g_theme.edit = RGB(43, 43, 43);
    } else {
        g_theme.window = GetSysColor(COLOR_WINDOW);
        g_theme.text = GetSysColor(COLOR_WINDOWTEXT);
        g_theme.hint = highContrast ? GetSysColor(COLOR_GRAYTEXT) : RGB(96, 96, 96);
        g_theme.edit = GetSysColor(COLOR_WINDOW);
    }
    g_theme.windowBrush = CreateSolidBrush(g_theme.window);
    g_theme.editBrush = CreateSolidBrush(g_theme.edit);
}
void InitDarkModeSupport() {
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    g_dark.setPreferredAppMode =
        reinterpret_cast<int (WINAPI*)(int)>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    g_dark.allowDarkForWindow =
        reinterpret_cast<BOOL (WINAPI*)(HWND, BOOL)>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
    g_dark.flushMenuThemes =
        reinterpret_cast<void (WINAPI*)()>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (g_dark.setPreferredAppMode) g_dark.setPreferredAppMode(1);
    if (g_dark.flushMenuThemes) g_dark.flushMenuThemes();
}
void ApplyChrome(HWND window) {
    BOOL dark = g_theme.dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
    if (g_dark.allowDarkForWindow) g_dark.allowDarkForWindow(window, dark);
}
void ApplyControlTheme(HWND control) {
    if (g_dark.allowDarkForWindow) g_dark.allowDarkForWindow(control, g_theme.dark ? TRUE : FALSE);
    wchar_t name[32]{};
    GetClassNameW(control, name, ARRAYSIZE(name));
    const wchar_t* theme = nullptr;
    if (g_theme.dark)
        theme = _wcsicmp(name, L"Edit") == 0 ? L"DarkMode_CFD" : L"DarkMode_Explorer";
    SetWindowTheme(control, theme, nullptr);
}
void ApplyThemeToWindow() {
    static bool applying{};
    if (applying) return;
    applying = true;
    LoadTheme();
    if (!g_window) {
        applying = false;
        return;
    }
    ApplyChrome(g_window);
    for (HWND child = GetWindow(g_window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        ApplyControlTheme(child);
    if (g_dark.flushMenuThemes) g_dark.flushMenuThemes();
    InvalidateRect(g_window, nullptr, TRUE);
    applying = false;
}

HFONT MakeFont(int points, int weight) {
    const HDC dc = GetDC(nullptr);
    const int height = -MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}
void CreateFonts() {
    g_font = MakeFont(9, FW_NORMAL);
    g_titleFont = MakeFont(13, FW_SEMIBOLD);
    if (!g_font) g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!g_titleFont) g_titleFont = g_font;
}
void DestroyFonts() {
    const auto stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (g_titleFont && g_titleFont != g_font && g_titleFont != stock) DeleteObject(g_titleFont);
    if (g_font && g_font != stock) DeleteObject(g_font);
    g_font = g_titleFont = nullptr;
}

std::filesystem::path DataDir() {
    wchar_t value[32768]{};
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", value, ARRAYSIZE(value));
    auto path = size && size < ARRAYSIZE(value)
        ? std::filesystem::path(value) / L"Redshift"
        : std::filesystem::temp_directory_path() / L"Redshift";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}
std::filesystem::path SettingsPath() { return DataDir() / L"settings.ini"; }
std::filesystem::path LogPath() { return DataDir() / L"launch.log"; }

std::wstring Text(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}
void Status(const std::wstring& text) { SetWindowTextW(g_status, text.c_str()); }
std::wstring ErrorText(DWORD code) {
    wchar_t* buffer{};
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? buffer : L"Windows error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}
std::filesystem::path ModuleDir() {
    wchar_t path[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return length && length < ARRAYSIZE(path) ? std::filesystem::path(path).parent_path() : std::filesystem::path();
}
bool IsX64(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    DWORD offset{}, signature{};
    WORD machine{};
    if (!file) return false;
    file.seekg(0x3c); file.read(reinterpret_cast<char*>(&offset), sizeof(offset));
    file.seekg(offset); file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&machine), sizeof(machine));
    return file && signature == IMAGE_NT_SIGNATURE && machine == IMAGE_FILE_MACHINE_AMD64;
}
bool HasTrustedSignature(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{sizeof(trust)};
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    return result == ERROR_SUCCESS;
}
std::wstring HookEventBase() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t value[96]{};
    if (StringFromGUID2(id, value, ARRAYSIZE(value)) <= 0) return {};
    return L"Local\\Redshift.Hook." + std::wstring(value);
}
class EnvironmentValue {
public:
    EnvironmentValue(const wchar_t* name, const std::wstring& value) : name_(name) {
        const DWORD required = GetEnvironmentVariableW(name_, nullptr, 0);
        if (required) {
            previous_.resize(required);
            const DWORD copied = GetEnvironmentVariableW(name_, previous_.data(), required);
            if (copied && copied < required) {
                previous_.resize(copied);
                hadPrevious_ = true;
            }
        } else {
            hadPrevious_ = GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        }
        set_ = SetEnvironmentVariableW(name_, value.c_str()) != FALSE;
    }
    ~EnvironmentValue() {
        if (set_) SetEnvironmentVariableW(name_, hadPrevious_ ? previous_.c_str() : nullptr);
    }
    bool Set() const { return set_; }

private:
    const wchar_t* name_{};
    std::wstring previous_;
    bool hadPrevious_{};
    bool set_{};
};
bool Fail(std::wstring& error, const std::wstring& message) {
    error = message;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wofstream log(LogPath(), std::ios::app);
    log << now.wYear << L'-' << now.wMonth << L'-' << now.wDay << L' '
        << now.wHour << L':' << now.wMinute << L':' << now.wSecond
        << L"  " << message << L'\n';
    return false;
}
void Save() {
    WritePrivateProfileStringW(L"discord", L"executable", Text(g_path).c_str(), SettingsPath().c_str());
}
void Load() {
    wchar_t saved[32768]{};
    GetPrivateProfileStringW(L"discord", L"executable", L"", saved, ARRAYSIZE(saved), SettingsPath().c_str());
    const auto resolved = ResolveDiscord(saved);
    SetWindowTextW(g_path, resolved.empty() ? saved : resolved.c_str());
}
void Browse() {
    wchar_t path[32768]{}; wcsncpy_s(path, Text(g_path).c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g_window; dialog.lpstrFilter = L"Discord.exe\0Discord.exe\0Executables\0*.exe\0";
    dialog.lpstrFile = path; dialog.nMaxFile = ARRAYSIZE(path);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) SetWindowTextW(g_path, path);
}
bool Open(const std::filesystem::path& path) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI; info.hwnd = g_window;
    info.lpVerb = L"open"; info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}
bool DiscordIsRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = Process32FirstW(snapshot, &entry) != FALSE;
    while (found && _wcsicmp(entry.szExeFile, L"Discord.exe") != 0)
        found = Process32NextW(snapshot, &entry) != FALSE;
    CloseHandle(snapshot); return found;
}
bool LaunchPath(std::filesystem::path target, std::wstring& error) {
    target = ResolveDiscord(target.wstring());
    if (!std::filesystem::is_regular_file(target) || _wcsicmp(target.filename().c_str(), L"Discord.exe")) {
        return Fail(error, L"Select Discord.exe.");
    }
    if (!IsX64(target)) return Fail(error, L"Not a 64-bit Discord.exe.");
    if (!HasTrustedSignature(target)) return Fail(error, L"Discord.exe is not signed.");
    if (DiscordIsRunning()) return Fail(error, L"Quit Discord first.");
    auto hook = ModuleDir() / L"RedshiftPrivacyHook.dll";
    if (!std::filesystem::is_regular_file(hook)) return Fail(error, L"RedshiftPrivacyHook.dll is missing.");
    BOOL substituted{};
    int bytes = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, hook.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ansi(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, hook.c_str(), -1, ansi.data(), bytes, nullptr, &substituted);
    if (substituted) return Fail(error, L"Hook path has unsupported characters.");
    std::wstring command = L"\"" + target.wstring() + L"\"";
    const std::wstring eventBase = HookEventBase();
    if (eventBase.empty()) return Fail(error, L"Couldn't initialize hook verification.");
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (eventBase + L".Ready").c_str());
    HANDLE failed = CreateEventW(nullptr, TRUE, FALSE, (eventBase + L".Failed").c_str());
    if (!ready || !failed) {
        if (ready) CloseHandle(ready);
        if (failed) CloseHandle(failed);
        return Fail(error, L"Couldn't initialize hook verification.");
    }
    EnvironmentValue statusEvent(kHookStatusVariable, eventBase);
    if (!statusEvent.Set()) {
        CloseHandle(ready); CloseHandle(failed);
        return Fail(error, L"Couldn't initialize hook verification.");
    }
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!DetourCreateProcessWithDllExW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, target.parent_path().c_str(), &startup, &process, ansi.c_str(), CreateProcessW)) {
        CloseHandle(ready); CloseHandle(failed);
        return Fail(error, L"Launch failed: " + ErrorText(GetLastError()));
    }
    HANDLE waits[]{ready, failed, process.hProcess};
    const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 10000);
    const bool verified = wait == WAIT_OBJECT_0;
    if (!verified && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, ERROR_DLL_INIT_FAILED);
    CloseHandle(ready); CloseHandle(failed);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (!verified) {
        const std::wstring message = wait == WAIT_OBJECT_0 + 1
            ? L"The privacy hook could not be installed."
            : wait == WAIT_OBJECT_0 + 2
                ? L"Discord exited before the privacy hook was installed."
                : L"Timed out while verifying the privacy hook.";
        return Fail(error, message);
    }
    return true;
}
bool Launch(std::wstring& error) {
    std::filesystem::path target = ResolveDiscord(Text(g_path));
    if (!target.empty()) SetWindowTextW(g_path, target.c_str());
    if (!LaunchPath(target, error)) return false;
    Save(); return true;
}
bool CreateShortcut(std::wstring& error) {
    const std::filesystem::path discord = ResolveDiscord(Text(g_path));
    if (!std::filesystem::is_regular_file(discord)) { error = L"Select Discord.exe first."; return false; }
    PWSTR desktopRaw{};
    HRESULT result = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &desktopRaw);
    if (FAILED(result)) { error = L"Couldn't find the Desktop."; return false; }
    const auto shortcut = std::filesystem::path(desktopRaw) / L"Discord (Redshift).lnk";
    CoTaskMemFree(desktopRaw);
    IShellLinkW* link{};
    result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(result)) { error = L"Couldn't create the shortcut."; return false; }
    const auto launcher = ModuleDir() / L"Redshift.exe";
    const std::wstring args = L"--launch \"" + discord.wstring() + L"\"";
    link->SetPath(launcher.c_str()); link->SetArguments(args.c_str());
    link->SetWorkingDirectory(launcher.parent_path().c_str());
    link->SetDescription(L"Launch Discord with Redshift");
    link->SetIconLocation(discord.c_str(), 0); link->SetShowCmd(SW_HIDE);
    IPersistFile* persist{};
    result = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(result)) { result = persist->Save(shortcut.c_str(), TRUE); persist->Release(); }
    link->Release();
    if (FAILED(result)) { error = L"Couldn't save the shortcut."; return false; }
    Save(); return true;
}
HWND Control(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD extra = 0) {
    HWND control = CreateWindowExW(extra, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
        g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    ApplyControlTheme(control);
    return control;
}
void CreateControls() {
    HWND title = Control(L"STATIC", L"Redshift", 0, 20, 16, 620, 24, 0);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
    Control(L"STATIC", L"Launch Discord with host process names filtered from its view.",
        0, 20, 42, 620, 20, 0);
    Control(L"STATIC", L"Discord executable", 0, 20, 76, 200, 18, 0);
    g_path = Control(L"EDIT", L"", ES_AUTOHSCROLL, 20, 98, 504, 26, IdPath, WS_EX_CLIENTEDGE);
    Control(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 532, 97, 108, 28, IdBrowse);
    g_hint = Control(L"STATIC", L"Quit Discord completely before launching. A running instance cannot be protected.",
        0, 20, 134, 620, 18, IdHint);
    Control(L"BUTTON", L"Launch protected Discord", BS_DEFPUSHBUTTON, 20, 166, 196, 30, IdLaunch);
    Control(L"BUTTON", L"Create desktop shortcut", BS_PUSHBUTTON, 224, 166, 178, 30, IdShortcut);
    Control(L"BUTTON", L"Open log", BS_PUSHBUTTON, 410, 166, 88, 30, IdLog);
    g_status = Control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 20, 210, 620, 36, IdStatus);
    Load();
    if (Text(g_path).empty()) Status(L"Select Discord.exe if it was not detected automatically.");
    else Status(L"Ready. Quit Discord, then launch.");
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        LoadTheme();
        ApplyChrome(window);
        CreateFonts();
        CreateControls();
        return 0;
    case WM_ERASEBKGND: {
        RECT area{};
        GetClientRect(window, &area);
        FillRect(reinterpret_cast<HDC>(wParam), &area, g_theme.windowBrush);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, reinterpret_cast<HWND>(lParam) == g_hint ? g_theme.hint : g_theme.text);
        return reinterpret_cast<LRESULT>(g_theme.windowBrush);
    }
    case WM_CTLCOLOREDIT: {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, g_theme.text);
        SetBkColor(dc, g_theme.edit);
        return reinterpret_cast<LRESULT>(g_theme.editBrush);
    }
    case WM_SETTINGCHANGE:
        if (lParam && _wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0)
            ApplyThemeToWindow();
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_THEMECHANGED:
        ApplyThemeToWindow();
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_COMMAND:
        if (LOWORD(wParam) == IdBrowse) Browse();
        else if (LOWORD(wParam) == IdLaunch) { std::wstring error; Status(Launch(error) ? L"Launched." : error); }
        else if (LOWORD(wParam) == IdShortcut) { std::wstring error; Status(CreateShortcut(error) ? L"Shortcut created." : error); }
        else if (LOWORD(wParam) == IdLog) Open(LogPath());
        return 0;
    case WM_CLOSE: Save(); DestroyWindow(window); return 0;
    case WM_DESTROY: DestroyFonts(); DestroyThemeBrushes(); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argumentCount{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount == 3 && _wcsicmp(arguments[1], L"--launch") == 0) {
        const std::filesystem::path target(arguments[2]);
        LocalFree(arguments);
        std::wstring error;
        if (LaunchPath(target, error)) return 0;
        MessageBoxW(nullptr, error.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return 2;
    }
    if (arguments) LocalFree(arguments);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Redshift.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    g_instance = instance;
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitDarkModeSupport();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = WindowProc; wc.hInstance = instance;
    wc.hIcon = wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_REDSHIFT));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = nullptr; wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return 1;
    constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT area{0, 0, 660, 258};
    AdjustWindowRectEx(&area, kWindowStyle, FALSE, 0);
    HWND window = CreateWindowExW(0, kClass, kTitle, kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        area.right - area.left, area.bottom - area.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (SUCCEEDED(com)) CoUninitialize();
    CloseHandle(mutex); return static_cast<int>(message.wParam);
}
