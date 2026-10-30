#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include "network.hpp"
#include <objidl.h> // COM declarations must precede GDI+ with the Windows SDK.

#include <atomic>
#include <commctrl.h>
#include <condition_variable>
#include <ctime>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gdiplus.h>
#include <mutex>
#include <shellapi.h>
#include <thread>
#include <windowsx.h>

using namespace Gdiplus;
namespace {
constexpr wchar_t kClass[] = L"CryptoTaskbar.Window.v1";
constexpr UINT kTray = WM_APP + 1, kUpdated = WM_APP + 2;
constexpr int kRefresh = 101, kSettings = 102, kStartup = 103, kExit = 104, kFolder = 105;
HINSTANCE g_instance;
HWND g_overlay{}, g_popup{}, g_editor{}, g_edit{};
HANDLE g_singleton{};
HFONT g_editFont{};
ULONG_PTR g_gdiToken{};
std::filesystem::path g_configPath;
std::mutex g_mutex;
std::condition_variable g_cv;
ct::Config g_config;
std::vector<ct::Quote> g_quotes;
std::thread g_worker;
bool g_stop = false, g_refresh = false;
unsigned g_generation = 0;
int g_scroll = 0, g_wheel = 0;
UINT g_taskbarCreated{};
bool g_dark = true, g_popupPinned = false, g_preview = false, g_inspect = false;
int g_previewTheme = 0;
DWORD g_hoverStart = 0, g_leaveStart = 0;
float g_scale = 1;
RECT g_barRect{};
NOTIFYICONDATAW g_tray{};

int Px(int n) { return MulDiv(n, static_cast<int>(g_scale * 96), 96); }
long long Now() { return static_cast<long long>(std::time(nullptr)); }
ct::Config Config() {
    std::lock_guard lock(g_mutex);
    return g_config;
}
std::vector<ct::Quote> Quotes() {
    std::lock_guard lock(g_mutex);
    return g_quotes;
}
std::wstring ReadText(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read config.json.");
    if (std::filesystem::file_size(path) > 256 * 1024)
        throw std::runtime_error("Config exceeds 256 KiB.");
    std::string text((std::istreambuf_iterator<char>(f)), {});
    if (text.size() > 256 * 1024)
        throw std::runtime_error("Config exceeds 256 KiB.");
    if (text.rfind("\xEF\xBB\xBF", 0) == 0)
        text.erase(0, 3);
    return ct::Wide(text);
}
void SaveText(const std::string &text) {
    std::filesystem::create_directories(g_configPath.parent_path());
    auto temp = g_configPath;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f)
            throw std::runtime_error("Cannot write config.json.");
    }
    if (std::filesystem::exists(g_configPath)) {
        auto backup = g_configPath;
        backup += L".bak";
        if (!CopyFileW(g_configPath.c_str(), backup.c_str(), FALSE))
            throw std::runtime_error("Cannot back up config.json.");
    }
    if (!MoveFileExW(temp.c_str(), g_configPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save config.json.");
}
void Apply(ct::Config config) {
    {
        std::lock_guard lock(g_mutex);
        g_config = std::move(config);
        g_quotes.assign(g_config.coins.size(), {});
        ++g_generation;
        g_refresh = true;
    }
    g_scroll = 0;
    g_cv.notify_all();
    PostMessageW(g_overlay, kUpdated, 0, 0);
}
void Refresh() {
    {
        std::lock_guard lock(g_mutex);
        g_refresh = true;
    }
    g_cv.notify_all();
}
void Worker() {
    auto session = ct::OpenSession();
    struct Cached {
        ct::Json body;
        std::string error;
    };
    std::map<std::string, long long> retryAfter;
    unsigned lastGeneration = 0;
    for (;;) {
        ct::Config c;
        std::vector<ct::Quote> quotes;
        unsigned generation;
        {
            std::lock_guard lock(g_mutex);
            if (g_stop)
                break;
            c = g_config;
            quotes = g_quotes;
            generation = g_generation;
            g_refresh = false;
        }
        if (lastGeneration != generation) {
            retryAfter.clear();
            lastGeneration = generation;
        }
        std::map<std::pair<std::string, std::string>, Cached> cache;
        for (size_t i = 0; i < c.coins.size(); ++i) {
            {
                std::lock_guard lock(g_mutex);
                if (g_stop || generation != g_generation)
                    break;
            }
            const auto &coin = c.coins[i];
            if (retryAfter[coin.source] > Now())
                continue;
            const auto key = std::make_pair(coin.source, ct::RequestUrl(c, coin));
            auto [it, inserted] = cache.try_emplace(key);
            if (inserted) {
                try {
                    if (!session) {
                        session = ct::OpenSession();
                        if (!session)
                            throw std::runtime_error("Cannot initialize networking.");
                    }
                    it->second.body =
                        ct::FetchJson(session.get(), key.second, c.sources.at(coin.source));
                } catch (const std::exception &e) {
                    it->second.error = e.what();
                }
            }
            try {
                if (!it->second.error.empty())
                    throw std::runtime_error(it->second.error);
                auto q = ct::ParseQuote(it->second.body, c.sources.at(coin.source), coin);
                q.updated = Now();
                quotes[i] = std::move(q);
            } catch (const std::exception &e) {
                quotes[i].error = e.what();
                quotes[i].stale = quotes[i].price.has_value();
            }
        }
        // Apply rate-limit backoff after all coins sharing a batched response get
        // the error.
        for (const auto &[key, result] : cache)
            if (result.error == "HTTP 429")
                retryAfter[key.first] = Now() + 60;
        {
            std::lock_guard lock(g_mutex);
            if (g_stop)
                break;
            if (generation == g_generation)
                g_quotes = std::move(quotes);
        }
        PostMessageW(g_overlay, kUpdated, 0, 0);
        std::unique_lock lock(g_mutex);
        g_cv.wait_for(lock, std::chrono::seconds(c.refreshSeconds),
                      [&] { return g_stop || g_refresh || g_generation != generation; });
        if (g_stop)
            break;
    }
}

bool DarkTheme() {
    auto c = Config();
    if (g_previewTheme)
        return g_previewTheme == 2;
    if (c.theme != "system")
        return c.theme == "dark";
    DWORD light = 0, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    return !light;
}
Color Fg() { return g_dark ? Color(255, 240, 242, 246) : Color(255, 24, 29, 37); }
Color Muted() { return g_dark ? Color(255, 162, 169, 181) : Color(255, 94, 103, 118); }
Color ChangeColor(double change) {
    return change >= 0 ? (g_dark ? Color(255, 83, 204, 157) : Color(255, 0, 119, 80))
                       : (g_dark ? Color(255, 248, 130, 142) : Color(255, 188, 50, 67));
}
void Text(Graphics &g, const std::wstring &text, double x, double y, double w, double h,
          double size, Color color, bool right = false, bool bold = false) {
    FontFamily family(L"Segoe UI");
    Font font(&family, static_cast<float>(size * g_scale), bold ? FontStyleBold : FontStyleRegular,
              UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(right ? StringAlignmentFar : StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font,
                 RectF(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                       static_cast<float>(h)),
                 &format, &brush);
}
void RoundRect(Graphics &g, RectF r, double radius, Color color) {
    GraphicsPath path;
    const float d = static_cast<float>(radius * 2);
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
}
std::wstring Price(const ct::Config &c, const ct::Quote &q) {
    return q.price ? ct::Wide(c.currency + ct::FormatPrice(*q.price)) : L"—";
}
int PopupRows() {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&g_barRect, MONITOR_DEFAULTTOPRIMARY), &mi);
    return std::max(1, std::min<int>(12, (mi.rcWork.bottom - mi.rcWork.top - Px(108)) / Px(54)));
}
void Render(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (!w || !h)
        return;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels{};
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(dc, dib);
    {
        Bitmap bitmap(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE *>(pixels));
        Graphics g(&bitmap);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const auto c = Config();
        const auto quotes = Quotes();
        if (hwnd == g_overlay) {
            const int count = std::min(c.visibleCoins, static_cast<int>(c.coins.size()));
            const float cell = static_cast<float>(w) / count;
            for (int i = 0; i < count; ++i) {
                const auto &q = quotes[i];
                const float x = i * cell + Px(9), cw = cell - Px(18);
                Text(g, ct::Wide(c.coins[i].label) + (q.stale ? L" ·" : L""), x, 0, cw, h * .43f,
                     10, Muted());
                Text(g, Price(c, q), x, h * .34f, cw, h * .6f, 13, q.stale ? Muted() : Fg(), false,
                     true);
                if (c.showChange && q.change)
                    Text(g, ct::Wide(ct::FormatChange(*q.change)), x, 0, cw, h * .43f, 10,
                         q.stale ? Muted() : ChangeColor(*q.change), true);
                if (i) {
                    Pen pen(g_dark ? Color(60, 160, 170, 185) : Color(50, 80, 90, 110));
                    g.DrawLine(&pen, i * cell, static_cast<float>(Px(12)), i * cell,
                               static_cast<float>(h - Px(12)));
                }
            }
        } else {
            RoundRect(g, RectF(1, 1, static_cast<float>(w - 2), static_cast<float>(h - 2)), Px(14),
                      g_dark ? Color(252, 31, 34, 40) : Color(253, 249, 250, 252));
            Text(g, L"自选行情", Px(20), Px(10), Px(160), Px(28), 13, Fg(), false, true);
            Text(g, L"24 小时", w - Px(100), Px(10), Px(80), Px(28), 10, Muted(), true);
            const int rows = std::min(PopupRows(), static_cast<int>(c.coins.size()));
            g_scroll =
                std::clamp(g_scroll, 0, std::max(0, static_cast<int>(c.coins.size()) - rows));
            for (int row = 0; row < rows; ++row) {
                const int i = row + g_scroll;
                const auto &q = quotes[i];
                const float y = static_cast<float>(Px(46 + row * 54));
                Text(g, ct::Wide(c.coins[i].label), Px(20), y, Px(95), Px(24), 13, Fg(), false,
                     true);
                std::wstring status = ct::Wide(c.coins[i].source);
                if (!q.error.empty())
                    status += L" · " + ct::Wide(q.error);
                else if (!q.price)
                    status += L" · 正在读取";
                Text(g, status, Px(20), y + Px(22), w - Px(130), Px(18), 10, Muted());
                Text(g, Price(c, q), Px(120), y, w - Px(140), Px(24), 13, q.stale ? Muted() : Fg(),
                     true, true);
                if (q.change)
                    Text(g, ct::Wide(ct::FormatChange(*q.change)), w - Px(110), y + Px(23), Px(90),
                         Px(18), 11, q.stale ? Muted() : ChangeColor(*q.change), true);
            }
            int errors = 0;
            long long oldest = Now();
            bool any = false;
            for (const auto &q : quotes) {
                errors += !q.error.empty();
                if (q.updated) {
                    oldest = std::min(oldest, q.updated);
                    any = true;
                }
            }
            std::wstring footer;
            if (any) {
                std::time_t t = oldest;
                tm local{};
                localtime_s(&local, &t);
                wchar_t time[20];
                wcsftime(time, 20, L"%H:%M:%S", &local);
                footer = L"更新于 " + std::wstring(time);
            } else
                footer = L"正在读取行情";
            if (errors)
                footer = any ? footer + (errors == static_cast<int>(quotes.size())
                                             ? L" · 数据未更新"
                                             : L" · 部分数据未更新")
                             : L"读取失败，请检查数据源或网络";
            Text(g, footer, Px(20), h - Px(32), w - Px(80), Px(22), 10, Muted());
            if (static_cast<int>(c.coins.size()) > rows)
                Text(g,
                     std::to_wstring(g_scroll + 1) + L"–" + std::to_wstring(g_scroll + rows) +
                         L" / " + std::to_wstring(c.coins.size()),
                     w - Px(100), h - Px(32), Px(80), Px(22), 10, Muted(), true);
        }
        g.Flush(FlushIntentionSync);
    }
    SIZE size{w, h};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, nullptr, &size, dc, &source, 0, &blend, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
bool Contains(HWND hwnd, POINT point) {
    RECT r{};
    return IsWindowVisible(hwnd) && GetWindowRect(hwnd, &r) && PtInRect(&r, point);
}
void ShowPopup(bool pinned) {
    g_popupPinned = pinned;
    const auto c = Config();
    const int rows = std::min(PopupRows(), static_cast<int>(c.coins.size()));
    const int width = Px(360), height = Px(46 + rows * 54 + 36);
    RECT overlay{};
    GetWindowRect(g_overlay, &overlay);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&overlay, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int x =
        std::clamp(overlay.left, mi.rcWork.left, std::max(mi.rcWork.left, mi.rcWork.right - width));
    const int y = std::clamp(g_barRect.top - height - Px(8), mi.rcWork.top,
                             std::max(mi.rcWork.top, mi.rcWork.bottom - height));
    SetWindowPos(g_popup, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Render(g_popup);
}
bool Fullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_overlay || fg == g_popup || fg == g_editor || fg == GetShellWindow())
        return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Progman") == 0)
        return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY) !=
        MonitorFromRect(&g_barRect, MONITOR_DEFAULTTOPRIMARY))
        return false;
    RECT r{};
    if (!GetWindowRect(fg, &r))
        return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
void Placement() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar || !GetWindowRect(taskbar, &g_barRect)) {
        ShowWindow(g_overlay, SW_HIDE);
        ShowWindow(g_popup, SW_HIDE);
        return;
    }
    const auto c = Config();
    const float scale = GetDpiForWindow(taskbar) / 96.f;
    const bool scaleChanged = g_scale != scale;
    g_scale = scale;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&g_barRect, MONITOR_DEFAULTTOPRIMARY), &mi);
    if ((!g_preview && Fullscreen()) || g_barRect.bottom - g_barRect.top > Px(100) ||
        g_barRect.top < mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top) / 2 ||
        g_barRect.top >= mi.rcMonitor.bottom - 2) {
        ShowWindow(g_overlay, SW_HIDE);
        ShowWindow(g_popup, SW_HIDE);
        return;
    }
    int x = g_barRect.left + Px(c.offset);
    if (c.position == "right") {
        RECT tray{};
        HWND trayWnd = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        const int boundary =
            trayWnd && GetWindowRect(trayWnd, &tray) ? tray.left : g_barRect.right - Px(220);
        x = boundary - Px(c.offset + c.width);
    }
    const int width = std::min<int>(Px(c.width), g_barRect.right - g_barRect.left);
    x = std::clamp<int>(x, g_barRect.left, std::max(g_barRect.left, g_barRect.right - width));
    const int height = std::min<int>(Px(46), g_barRect.bottom - g_barRect.top);
    SetWindowPos(g_overlay, HWND_TOPMOST, x,
                 g_barRect.top + (g_barRect.bottom - g_barRect.top - height) / 2, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    bool dark = DarkTheme();
    bool changed = dark != g_dark;
    g_dark = dark;
    if (changed || scaleChanged) {
        Render(g_overlay);
        if (IsWindowVisible(g_popup))
            ShowPopup(g_popupPinned);
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    const bool over = Contains(g_overlay, cursor), overPopup = Contains(g_popup, cursor);
    if (over) {
        g_leaveStart = 0;
        if (!g_hoverStart)
            g_hoverStart = GetTickCount();
        if (GetTickCount() - g_hoverStart >= 300 && !IsWindowVisible(g_popup))
            ShowPopup(false);
    } else {
        g_hoverStart = 0;
        if (overPopup)
            g_leaveStart = 0;
        else if (IsWindowVisible(g_popup) && !g_popupPinned) {
            if (!g_leaveStart)
                g_leaveStart = GetTickCount();
            else if (GetTickCount() - g_leaveStart >= 350)
                ShowWindow(g_popup, SW_HIDE);
        }
    }
}

std::wstring ExePath() {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    return path;
}
bool StartupEnabled() {
    wchar_t value[32768]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"CryptoTaskbar", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}
void ToggleStartup() {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (StartupEnabled())
        RegDeleteValueW(key, L"CryptoTaskbar");
    else {
        const auto cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, L"CryptoTaskbar", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}
void OpenEditor();
void Menu() {
    g_popupPinned = false;
    ShowWindow(g_popup, SW_HIDE);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kRefresh, L"立即刷新");
    AppendMenuW(menu, MF_STRING, kSettings, L"配置…");
    AppendMenuW(menu, MF_STRING, kFolder, L"打开配置文件夹");
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), kStartup, L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, L"退出");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g_overlay);
    const auto id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x,
                                   point.y, 0, g_overlay, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_overlay, WM_NULL, 0, 0);
    switch (id) {
    case kRefresh:
        Refresh();
        break;
    case kSettings:
        OpenEditor();
        break;
    case kStartup:
        ToggleStartup();
        break;
    case kFolder:
        ShellExecuteW(nullptr, L"open", g_configPath.parent_path().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        break;
    case kExit:
        PostMessageW(g_overlay, WM_CLOSE, 0, 0);
        break;
    }
}
HICON MakeIcon() {
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32;
    bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *pixels{};
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(dc, bmp);
    {
        Bitmap b(32, 32, 128, PixelFormat32bppPARGB, static_cast<BYTE *>(pixels));
        Graphics g(&b);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush bg(Color(255, 54, 120, 238));
        g.FillEllipse(&bg, 1, 1, 30, 30);
        Pen pen(Color(255, 255, 255, 255), 2.8f);
        PointF line[]{{8, 22}, {13, 16}, {18, 19}, {24, 10}};
        g.DrawLines(&pen, line, 4);
        g.Flush();
    }
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, nullptr);
    ICONINFO iconInfo{TRUE, 0, 0, mask, bmp};
    HICON icon = CreateIconIndirect(&iconInfo);
    SelectObject(dc, old);
    DeleteObject(mask);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}
void AddTray() {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_overlay;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = kTray;
    if (!g_tray.hIcon)
        g_tray.hIcon = MakeIcon();
    wcscpy_s(g_tray.szTip, L"Crypto Taskbar · 自选行情");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}
void EditorTheme() {
    if (!g_editor)
        return;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_editor, 20, &dark, sizeof(dark));
    InvalidateRect(g_editor, nullptr, TRUE);
}
LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(w);
        RECT r{};
        GetClientRect(hwnd, &r);
        SetDCBrushColor(dc, g_dark ? RGB(31, 34, 40) : RGB(249, 250, 252));
        FillRect(dc, &r, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return 1;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(w);
        const COLORREF bg = g_dark ? RGB(31, 34, 40) : RGB(255, 255, 255);
        SetTextColor(dc, g_dark ? RGB(240, 242, 246) : RGB(24, 29, 37));
        SetBkColor(dc, bg);
        SetDCBrushColor(dc, bg);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_SIZE: {
        const int width = LOWORD(l), height = HIWORD(l);
        MoveWindow(g_edit, 16, 16, width - 32, height - 76, TRUE);
        MoveWindow(GetDlgItem(hwnd, 1), width - 190, height - 45, 80, 28, TRUE);
        MoveWindow(GetDlgItem(hwnd, 2), width - 100, height - 45, 80, 28, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 1) {
            int n = GetWindowTextLengthW(g_edit);
            std::wstring value(n + 1, 0);
            GetWindowTextW(g_edit, value.data(), n + 1);
            value.resize(n);
            try {
                auto text = ct::Utf8(value);
                auto config = ct::ParseConfig(text);
                SaveText(text);
                Apply(std::move(config));
                DestroyWindow(hwnd);
            } catch (const std::exception &e) {
                MessageBoxW(hwnd, ct::Wide(e.what()).c_str(), L"配置未保存",
                            MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        if (LOWORD(w) == 2) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {520, 360};
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_editor = nullptr;
        g_edit = nullptr;
        if (g_editFont) {
            DeleteObject(g_editFont);
            g_editFont = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
void OpenEditor() {
    if (g_editor) {
        ShowWindow(g_editor, SW_RESTORE);
        SetForegroundWindow(g_editor);
        return;
    }
    WNDCLASSW cls{};
    cls.lpfnWndProc = EditorProc;
    cls.hInstance = g_instance;
    cls.lpszClassName = L"CryptoTaskbar.Config";
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    g_editor = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, L"Crypto Taskbar — 配置",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 660, nullptr,
                               nullptr, g_instance, nullptr);
    if (!g_editor) {
        MessageBoxW(g_overlay,
                    (L"Cannot create settings window: " + std::to_wstring(GetLastError())).c_str(),
                    L"Crypto Taskbar", MB_OK);
        return;
    }
    g_edit =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_LEFT |
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        0, 0, 0, 0, g_editor, reinterpret_cast<HMENU>(10), g_instance, nullptr);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 256 * 1024, 0);
    g_editFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Consolas");
    SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    try {
        const auto text =
            ct::Replace(ct::Replace(ct::Utf8(ReadText(g_configPath)), "\r\n", "\n"), "\n", "\r\n");
        SetWindowTextW(g_edit, ct::Wide(text).c_str());
    } catch (...) {
        SetWindowTextW(g_edit, ct::Wide(ct::Replace(ct::DefaultConfig(), "\n", "\r\n")).c_str());
    }
    CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0,
                  0, 0, g_editor, reinterpret_cast<HMENU>(1), g_instance, nullptr);
    CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, g_editor,
                  reinterpret_cast<HMENU>(2), g_instance, nullptr);
    RECT r{};
    GetClientRect(g_editor, &r);
    SendMessageW(g_editor, WM_SIZE, 0, MAKELPARAM(r.right, r.bottom));
    EditorTheme();
    SetWindowPos(g_editor, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_editor);
    SetFocus(g_edit);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        AddTray();
        Placement();
        Render(g_overlay);
        return 0;
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return (hwnd == g_popup || g_preview || g_inspect) ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (hwnd == g_overlay) {
            Placement();
            if (g_preview && w == 2) {
                ShowPopup(true);
                KillTimer(hwnd, 2);
            }
        }
        return 0;
    case kUpdated:
        Placement();
        Render(g_overlay);
        if (IsWindowVisible(g_popup))
            ShowPopup(g_popupPinned);
        EditorTheme();
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (hwnd == g_overlay) {
            Placement();
            Render(g_overlay);
            if (IsWindowVisible(g_popup))
                ShowPopup(g_popupPinned);
            EditorTheme();
        }
        return 0;
    case WM_LBUTTONUP:
        if (hwnd == g_overlay) {
            if (IsWindowVisible(g_popup) && g_popupPinned) {
                g_popupPinned = false;
                ShowWindow(g_popup, SW_HIDE);
            } else
                ShowPopup(true);
        }
        return 0;
    case WM_RBUTTONUP:
        Menu();
        return 0;
    case WM_MOUSEWHEEL:
        if (hwnd == g_popup) {
            g_wheel += GET_WHEEL_DELTA_WPARAM(w);
            int steps = g_wheel / WHEEL_DELTA;
            g_wheel %= WHEEL_DELTA;
            g_scroll -= steps * 3;
            Render(g_popup);
        }
        return 0;
    case WM_KEYDOWN:
        if (hwnd == g_popup) {
            if (w == VK_ESCAPE) {
                g_popupPinned = false;
                ShowWindow(g_popup, SW_HIDE);
            } else {
                if (w == VK_DOWN)
                    ++g_scroll;
                if (w == VK_UP)
                    --g_scroll;
                if (w == VK_NEXT)
                    g_scroll += PopupRows();
                if (w == VK_PRIOR)
                    g_scroll -= PopupRows();
                if (w == VK_HOME)
                    g_scroll = 0;
                if (w == VK_END)
                    g_scroll = 100;
                Render(g_popup);
            }
        }
        return 0;
    case kTray:
        if (l == WM_RBUTTONUP)
            Menu();
        else if (l == WM_LBUTTONUP) {
            Placement();
            ShowPopup(true);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(g_overlay, SW_HIDE);
        ShowWindow(g_popup, SW_HIDE);
        if (g_editor)
            DestroyWindow(g_editor);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path custom;
    bool editOnStart = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--config" && i + 1 < argc)
            custom = argv[++i];
        else if (arg == L"--preview-dark") {
            g_preview = true;
            g_previewTheme = 2;
        } else if (arg == L"--preview-light") {
            g_preview = true;
            g_previewTheme = 1;
        } else if (arg == L"--settings")
            editOnStart = true;
        else if (arg == L"--inspect")
            g_inspect = true;
    }
    LocalFree(argv);
    g_singleton = CreateMutexW(
        nullptr, FALSE, g_preview ? L"Local\\CryptoTaskbar.Preview" : L"Local\\CryptoTaskbar.v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND previous = FindWindowW(kClass, L"Crypto Taskbar");
        if (previous)
            PostMessageW(previous, kTray, 0, WM_LBUTTONUP);
        if (g_singleton)
            CloseHandle(g_singleton);
        return 0;
    }
    try {
        if (!custom.empty())
            g_configPath = std::filesystem::absolute(custom);
        else {
            wchar_t data[32768]{};
            if (!GetEnvironmentVariableW(L"LOCALAPPDATA", data, 32768))
                throw std::runtime_error("LOCALAPPDATA is unavailable.");
            g_configPath = std::filesystem::path(data) / L"CryptoTaskbar" / L"config.json";
        }
        if (!std::filesystem::exists(g_configPath))
            SaveText(ct::DefaultConfig());
        g_config = ct::ParseConfig(ct::Utf8(ReadText(g_configPath)));
        g_quotes.resize(g_config.coins.size());
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, (ct::Wide(e.what()) + L"\n\n" + g_configPath.wstring()).c_str(),
                    L"Crypto Taskbar — 配置错误", MB_OK | MB_ICONERROR);
        if (g_singleton)
            CloseHandle(g_singleton);
        return 1;
    }
    GdiplusStartupInput startup;
    if (GdiplusStartup(&g_gdiToken, &startup, nullptr) != Ok)
        return 1;
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls))
        return 1;
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED |
            ((g_preview || g_inspect) ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE),
        kClass, L"Crypto Taskbar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    g_popup = CreateWindowExW(
        WS_EX_LAYERED | ((g_preview || g_inspect) ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW), kClass,
        L"Crypto Taskbar — 自选行情", WS_POPUP, 0, 0, 1, 1,
        (g_preview || g_inspect) ? nullptr : g_overlay, nullptr, instance, nullptr);
    if (!g_overlay || !g_popup)
        return 1;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_dark = DarkTheme();
    Placement();
    Render(g_overlay);
    AddTray();
    SetTimer(g_overlay, 1, 150, nullptr);
    if (g_preview) {
        for (size_t i = 0; i < g_quotes.size(); ++i) {
            g_quotes[i].price = (i == 0 ? 78485.12 : i == 1 ? 2246.34 : 98.04);
            g_quotes[i].change = (i == 1 ? 2.89 : -1.88);
            g_quotes[i].updated = Now();
        }
        Render(g_overlay);
        SetTimer(g_overlay, 2, 350, nullptr);
    } else
        g_worker = std::thread(Worker);
    if (editOnStart)
        OpenEditor();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_editor && IsDialogMessageW(g_editor, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    if (g_tray.hIcon)
        DestroyIcon(g_tray.hIcon);
    {
        std::lock_guard lock(g_mutex);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    DestroyWindow(g_popup);
    DestroyWindow(g_overlay);
    UnregisterClassW(kClass, instance);
    GdiplusShutdown(g_gdiToken);
    if (g_singleton)
        CloseHandle(g_singleton);
    return 0;
}
