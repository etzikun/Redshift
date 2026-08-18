#include "resource.h"
#include "discord_install.h"
#include "privacy_policy.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <detours.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <string>

namespace {
constexpr wchar_t kClass[] = L"RedshiftWindow";
constexpr wchar_t kTitle[] = L"Redshift";
constexpr wchar_t kDiscordRootVariable[] = L"REDSHIFT_DISCORD_ROOT";
constexpr wchar_t kLaunchStatusVariable[] = L"REDSHIFT_LAUNCH_STATUS";
constexpr DWORD kHookVerificationTimeoutMs = 30000;
enum { IdPath = 1001, IdBrowse, IdLaunch, IdStatus, IdHint, IdTheme };
HINSTANCE g_instance{};
HWND g_window{}, g_path{}, g_status{}, g_hint{}, g_themeChoice{};
HFONT g_font{}, g_titleFont{};
std::wstring g_startupError;

struct {
    bool dark{};
    COLORREF window{};
    COLORREF text{};
    COLORREF hint{};
    COLORREF edit{};
    HBRUSH windowBrush{};
    HBRUSH editBrush{};
} g_theme{};

void DestroyThemeBrushes() {
    if (g_theme.windowBrush) DeleteObject(g_theme.windowBrush);
    if (g_theme.editBrush) DeleteObject(g_theme.editBrush);
    g_theme.windowBrush = g_theme.editBrush = nullptr;
}
void LoadTheme() {
    DestroyThemeBrushes();
    if (g_theme.dark) {
        g_theme.window = RGB(32, 32, 32);
        g_theme.text = RGB(250, 250, 250);
        g_theme.hint = RGB(166, 166, 166);
        g_theme.edit = RGB(43, 43, 43);
    } else {
        g_theme.window = GetSysColor(COLOR_WINDOW);
        g_theme.text = GetSysColor(COLOR_WINDOWTEXT);
        g_theme.hint = RGB(96, 96, 96);
        g_theme.edit = GetSysColor(COLOR_WINDOW);
    }
    g_theme.windowBrush = CreateSolidBrush(g_theme.window);
    g_theme.editBrush = CreateSolidBrush(g_theme.edit);
}
void ApplyChrome(HWND window) {
    BOOL dark = g_theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
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
    InvalidateRect(g_window, nullptr, TRUE);
    applying = false;
}

HFONT MakeFont(int points, int weight, int dpi) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}
void CreateFonts() {
    const HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    g_font = MakeFont(9, FW_NORMAL, dpi);
    g_titleFont = MakeFont(13, FW_SEMIBOLD, dpi);
    if (!g_font) g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!g_titleFont) g_titleFont = g_font;
}
void DestroyFonts() {
    const auto stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (g_titleFont && g_titleFont != g_font && g_titleFont != stock) DeleteObject(g_titleFont);
    if (g_font && g_font != stock) DeleteObject(g_font);
    g_font = g_titleFont = nullptr;
}

const std::filesystem::path& ModuleDir() {
    static const std::filesystem::path path = [] {
        wchar_t value[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, value, ARRAYSIZE(value));
        return length && length < ARRAYSIZE(value)
            ? std::filesystem::path(value).parent_path() : std::filesystem::path();
    }();
    return path;
}
const std::filesystem::path& EnsureDataDirectory() {
    static const std::filesystem::path path = [] {
        wchar_t value[MAX_PATH]{};
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", value, ARRAYSIZE(value));
        auto dir = size && size < ARRAYSIZE(value)
            ? std::filesystem::path(value) / L"Redshift"
            : std::filesystem::temp_directory_path() / L"Redshift";
        std::error_code error;
        // SIDE EFFECT: ensure the local settings/log directory exists.
        std::filesystem::create_directories(dir, error);
        return dir;
    }();
    return path;
}
std::filesystem::path SettingsPath() { return EnsureDataDirectory() / L"settings.ini"; }
std::filesystem::path LogPath() { return EnsureDataDirectory() / L"launch.log"; }
void LoadThemePreference() {
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"appearance", L"theme", L"light", value, ARRAYSIZE(value),
        SettingsPath().c_str());
    g_theme.dark = _wcsicmp(value, L"dark") == 0;
}
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
bool IsX64(const std::filesystem::path& path) {
    DWORD type{};
    return GetBinaryTypeW(path.c_str(), &type) != FALSE && type == SCS_64BIT_BINARY;
}
std::wstring LaunchEventBase() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t value[96]{};
    if (StringFromGUID2(id, value, ARRAYSIZE(value)) <= 0) return {};
    return L"Local\\Redshift.Launch." + std::wstring(value);
}
class ScopedEnvironmentVariableChange {
public:
    ScopedEnvironmentVariableChange(const wchar_t* name, const std::wstring& value) : name_(name) {
        // SIDE EFFECT: set a launch-scoped environment variable.
        set_ = SetEnvironmentVariableW(name_, value.c_str()) != FALSE;
    }
    ~ScopedEnvironmentVariableChange() {
        // SIDE EFFECT: clear the launch-scoped environment variable.
        if (set_) SetEnvironmentVariableW(name_, nullptr);
    }
    bool WasSet() const { return set_; }

private:
    const wchar_t* name_{};
    bool set_{};
};
bool SetLaunchErrorAndAppendLog(std::wstring& error, const std::wstring& message) {
    error = message;
    // SIDE EFFECT: append one explicit launch error to launch.log.
    HANDLE log = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t line[1024]{};
        const int length = swprintf_s(line, L"%u-%u-%u %u:%u:%u  %s\r\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, message.c_str());
        if (length > 0) {
            DWORD written{};
            WriteFile(log, line, static_cast<DWORD>(length) * sizeof(wchar_t), &written, nullptr);
        }
        CloseHandle(log);
    }
    return false;
}
void WriteSettings() {
    const auto path = SettingsPath();
    // SIDE EFFECT: write the complete local settings policy.
    WritePrivateProfileStringW(L"discord", L"executable", Text(g_path).c_str(), path.c_str());
    WritePrivateProfileStringW(L"appearance", L"theme", g_theme.dark ? L"dark" : L"light",
        path.c_str());
}
std::filesystem::path SavedDiscordPath() {
    wchar_t saved[MAX_PATH]{};
    GetPrivateProfileStringW(L"discord", L"executable", L"", saved, ARRAYSIZE(saved), SettingsPath().c_str());
    return saved;
}
void LoadSettingsIntoControls() {
    const auto saved = SavedDiscordPath();
    const auto resolved = ResolveDiscord(saved);
    SetWindowTextW(g_path, resolved.empty() ? saved.c_str() : resolved.c_str());
}
void Browse() {
    wchar_t path[MAX_PATH]{}; wcsncpy_s(path, Text(g_path).c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g_window; dialog.lpstrFilter = L"Discord.exe\0Discord.exe\0Executables\0*.exe\0";
    dialog.lpstrFile = path; dialog.nMaxFile = ARRAYSIZE(path);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) SetWindowTextW(g_path, path);
}
bool DiscordIsRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = Process32FirstW(snapshot, &entry) != FALSE;
    while (found && !privacy_policy::IsDiscordExecutableName(entry.szExeFile))
        found = Process32NextW(snapshot, &entry) != FALSE;
    CloseHandle(snapshot); return found;
}
bool LaunchDiscordWithPrivacyHook(std::filesystem::path target, std::wstring& error) {
    target = ResolveDiscord(target);
    if (g_path && !target.empty()) SetWindowTextW(g_path, target.c_str());
    if (!std::filesystem::is_regular_file(target) ||
        !privacy_policy::IsDiscordExecutableName(target.native())) {
        return SetLaunchErrorAndAppendLog(error, L"Select Discord.exe.");
    }
    if (!IsX64(target)) return SetLaunchErrorAndAppendLog(error, L"Not a 64-bit Discord.exe.");
    const auto discordRoot = target.parent_path().parent_path();
    const auto expectedRoot = DefaultDiscordRoot();
    if (!privacy_policy::IsVerifiedDiscordInstallationRoot(
            discordRoot.native(), expectedRoot.parent_path().native())) {
        return SetLaunchErrorAndAppendLog(error, L"Discord must be installed under %LOCALAPPDATA%\\Discord.");
    }
    if (!privacy_policy::ShouldInjectInto(target.native(), discordRoot.native()))
        return SetLaunchErrorAndAppendLog(error, L"Discord.exe must be inside an app-X.Y.Z directory.");
    if (DiscordIsRunning()) return SetLaunchErrorAndAppendLog(error, L"Quit Discord first.");
    auto hook = ModuleDir() / L"RedshiftPrivacyHook.dll";
    if (!std::filesystem::is_regular_file(hook))
        return SetLaunchErrorAndAppendLog(error, L"RedshiftPrivacyHook.dll is missing.");
    BOOL substituted{};
    int bytes = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, hook.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ansi(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, hook.c_str(), -1, ansi.data(), bytes, nullptr, &substituted);
    if (substituted) return SetLaunchErrorAndAppendLog(error, L"Filter path has unsupported characters.");
    std::wstring command = L"\"" + target.wstring() + L"\"";
    const std::wstring eventBase = LaunchEventBase();
    if (eventBase.empty())
        return SetLaunchErrorAndAppendLog(error, L"Couldn't initialize launch verification.");
    wchar_t readyName[128]{};
    if (swprintf_s(readyName, L"%s.Ready", eventBase.c_str()) < 0)
        return SetLaunchErrorAndAppendLog(error, L"Couldn't initialize launch verification.");
    // SIDE EFFECT: create the named launch-verification event.
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!ready) {
        return SetLaunchErrorAndAppendLog(error, L"Couldn't initialize launch verification.");
    }
    ScopedEnvironmentVariableChange statusEvent(kLaunchStatusVariable, eventBase);
    ScopedEnvironmentVariableChange discordRootValue(kDiscordRootVariable, discordRoot.native());
    if (!statusEvent.WasSet() || !discordRootValue.WasSet()) {
        CloseHandle(ready);
        return SetLaunchErrorAndAppendLog(error, L"Couldn't initialize launch verification.");
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    // SIDE EFFECT: launch Discord with RedshiftPrivacyHook.dll injected.
    if (!DetourCreateProcessWithDllExW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, target.parent_path().c_str(), &startup, &process, ansi.c_str(), CreateProcessW)) {
        CloseHandle(ready);
        return SetLaunchErrorAndAppendLog(error, L"Launch failed: " + ErrorText(GetLastError()));
    }
    HANDLE waits[]{ready, process.hProcess};
    const DWORD wait = WaitForMultipleObjects(
        ARRAYSIZE(waits), waits, FALSE, kHookVerificationTimeoutMs);
    const bool verified = wait == WAIT_OBJECT_0;
    // SIDE EFFECT: terminate only the child we launched if hook verification failed.
    if (!verified && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, ERROR_DLL_INIT_FAILED);
    CloseHandle(ready);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (!verified) {
        const std::wstring message = wait == WAIT_OBJECT_0 + 1
            ? L"Discord exited before the privacy filter was installed."
            : L"Timed out while verifying the privacy filter.";
        return SetLaunchErrorAndAppendLog(error, message);
    }
    return true;
}
bool LaunchSelectedDiscordWithPrivacyHook(std::wstring& error) {
    if (!LaunchDiscordWithPrivacyHook(Text(g_path), error)) return false;
    WriteSettings(); return true;
}
HWND Control(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD extra = 0) {
    HWND control = CreateWindowExW(extra, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
        g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
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
    Control(L"STATIC", L"Theme", 0, 340, 172, 48, 18, 0);
    g_themeChoice = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
        390, 168, 112, 160, IdTheme);
    SendMessageW(g_themeChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light"));
    SendMessageW(g_themeChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
    SendMessageW(g_themeChoice, CB_SETCURSEL, g_theme.dark ? 1 : 0, 0);
    g_status = Control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 20, 210, 620, 36, IdStatus);
    LoadSettingsIntoControls();
    if (!g_startupError.empty()) Status(g_startupError);
    else if (Text(g_path).empty()) Status(L"Select Discord.exe if it was not detected automatically.");
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
    case WM_THEMECHANGED:
        ApplyThemeToWindow();
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_COMMAND:
        if (LOWORD(wParam) == IdBrowse) Browse();
        else if (LOWORD(wParam) == IdLaunch) {
            std::wstring error;
            Status(LaunchSelectedDiscordWithPrivacyHook(error) ? L"Launched." : error);
        }
        else if (LOWORD(wParam) == IdTheme && HIWORD(wParam) == CBN_SELCHANGE) {
            g_theme.dark = SendMessageW(g_themeChoice, CB_GETCURSEL, 0, 0) == 1;
            WriteSettings();
            ApplyThemeToWindow();
        }
        return 0;
    case WM_CLOSE: WriteSettings(); DestroyWindow(window); return 0;
    case WM_DESTROY: DestroyFonts(); DestroyThemeBrushes(); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ PWSTR, _In_ int show) {
    int argumentCount{};
    bool quickLaunchFailed = false;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && (argumentCount == 2 || argumentCount == 3) &&
        _wcsicmp(arguments[1], L"--launch") == 0) {
        const std::filesystem::path target = argumentCount == 3
            ? std::filesystem::path(arguments[2])
            : SavedDiscordPath();
        if (LaunchDiscordWithPrivacyHook(target, g_startupError)) {
            LocalFree(arguments);
            return 0;
        }
        quickLaunchFailed = true;
    }
    if (arguments) LocalFree(arguments);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Redshift.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    g_instance = instance;
    LoadThemePreference();
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
    ShowWindow(window, quickLaunchFailed ? SW_SHOWNORMAL : show); UpdateWindow(window);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    CloseHandle(mutex); return static_cast<int>(message.wParam);
}
