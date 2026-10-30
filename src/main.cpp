#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include "network.hpp"
#include <objidl.h> // COM declarations must precede GDI+ with the Windows SDK.

#include <atomic>
#include <commctrl.h>
#include <commdlg.h>
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
#include <uxtheme.h>
#include <windowsx.h>

using namespace Gdiplus;
namespace {
constexpr wchar_t kClass[] = L"CryptoTaskbar.Window.v1";
constexpr UINT kShowList = WM_APP + 1, kUpdated = WM_APP + 2;
constexpr int kRefresh = 101, kSettings = 102, kStartup = 103, kExit = 104, kFolder = 105;
HINSTANCE g_instance;
HWND g_overlay{}, g_popup{}, g_editor{};
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
bool g_dark = true, g_popupPinned = false, g_preview = false;
int g_previewTheme = 0;
DWORD g_hoverStart = 0, g_leaveStart = 0;
float g_scale = 1;
RECT g_barRect{};
size_t g_currentCoin = 0;
ULONGLONG g_rotatedAt = 0;
bool g_animating = false, g_menuOpen = false;

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
    g_currentCoin = 0;
    g_animating = false;
    g_rotatedAt = GetTickCount64();
    KillTimer(g_overlay, 3);
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
            if (retryAfter[coin.source] > Now()) {
                quotes[i].stale = quotes[i].price.has_value();
                continue;
            }
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
                const auto &source = c.sources.at(coin.source);
                auto q = ct::ParseQuote(it->second.body, source, coin, c.changePeriod);
                if (c.changePeriod != "24h" && !source.historyUrl.empty()) {
                    try {
                        const auto historyKey = std::make_pair(
                            coin.source, ct::RequestUrl(c, coin, true, Now() * 1000));
                        auto [entry, fresh] = cache.try_emplace(historyKey);
                        if (fresh) {
                            try {
                                entry->second.body =
                                    ct::FetchJson(session.get(), historyKey.second, source);
                            } catch (const std::exception &e) {
                                entry->second.error = e.what();
                            }
                        }
                        if (!entry->second.error.empty())
                            throw std::runtime_error(entry->second.error);
                        ct::ApplyHistoricalChange(q, entry->second.body, source, coin);
                    } catch (const std::exception &e) {
                        q.change.reset();
                        q.changeError = e.what();
                    }
                }
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
    const auto c = Config();
    const auto value = std::stoul((change >= 0 ? c.upColor : c.downColor).substr(1), nullptr, 16);
    return Color(255, static_cast<BYTE>(value >> 16), static_cast<BYTE>(value >> 8),
                 static_cast<BYTE>(value));
}
void Text(Graphics &g, const std::wstring &text, double x, double y, double w, double h,
          double size, Color color, bool right = false, bool bold = false, float scale = 0) {
    FontFamily family(L"Segoe UI");
    Font font(&family, static_cast<float>(size * (scale > 0 ? scale : g_scale)),
              bold ? FontStyleBold : FontStyleRegular, UnitPixel);
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
            g.Clear(Color(1, 0, 0, 0)); // Nonzero alpha makes the whole rectangle clickable.
            g_currentCoin %= c.coins.size();
            auto drawCoin = [&](size_t i, double y) {
                const auto &q = quotes[i];
                const int lineWidth = w - Px(18);
                Text(g, ct::Wide(c.coins[i].label) + (q.stale ? L" ·" : L""), Px(9), y, lineWidth,
                     h * .43, 10, Muted());
                const auto price = Price(c, q);
                double priceWidth = lineWidth;
                std::wstring change;
                double changeWidth = 0;
                if (c.showChange && q.change) {
                    change = ct::Wide(ct::FormatChange(*q.change));
                    FontFamily family(L"Segoe UI");
                    Font priceFont(&family, 13 * g_scale, FontStyleBold, UnitPixel);
                    Font changeFont(&family, 11 * g_scale, FontStyleRegular, UnitPixel);
                    RectF priceBounds, changeBounds;
                    g.MeasureString(price.c_str(), -1, &priceFont, PointF(0, 0), &priceBounds);
                    g.MeasureString(change.c_str(), -1, &changeFont, PointF(0, 0), &changeBounds);
                    changeWidth = std::min<double>(lineWidth, std::ceil(changeBounds.Width));
                    priceWidth = std::min<double>(std::ceil(priceBounds.Width),
                                                  std::max(0.0, lineWidth - changeWidth - Px(6)));
                }
                Text(g, price, Px(9), y + h * .34, priceWidth, h * .6, 13, q.stale ? Muted() : Fg(),
                     false, true);
                if (!change.empty())
                    Text(g, change, Px(9) + priceWidth + Px(6), y + h * .34, changeWidth, h * .6,
                         11, q.stale ? Muted() : ChangeColor(*q.change));
            };
            if (g_animating && c.coins.size() > 1) {
                double p = std::clamp((GetTickCount64() - g_rotatedAt) / 260.0, 0.0, 1.0);
                p = p * p * (3 - 2 * p);
                drawCoin(g_currentCoin, -p * h);
                drawCoin((g_currentCoin + 1) % c.coins.size(), (1 - p) * h);
            } else
                drawCoin(g_currentCoin, 0);
        } else {
            RoundRect(g, RectF(1, 1, static_cast<float>(w - 2), static_cast<float>(h - 2)), Px(14),
                      g_dark ? Color(252, 31, 34, 40) : Color(253, 249, 250, 252));
            Text(g, L"自选行情", Px(20), Px(10), Px(160), Px(28), 13, Fg(), false, true);
            Text(g, ct::Wide(c.changePeriod), w - Px(100), Px(10), Px(80), Px(28), 10, Muted(),
                 true);
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
                else if (!q.changeError.empty())
                    status += L" · " + ct::Wide(q.changeError);
                else if (!q.price)
                    status += L" · 正在读取";
                Text(g, status, Px(20), y + Px(22), w - Px(130), Px(18), 10, Muted());
                Text(g, Price(c, q), Px(120), y, w - Px(140), Px(24), 13, q.stale ? Muted() : Fg(),
                     true, true);
                if (q.change)
                    Text(g, ct::Wide(ct::FormatChange(*q.change)), w - Px(110), y + Px(23), Px(90),
                         Px(18), 11, q.stale ? Muted() : ChangeColor(*q.change), true);
                else
                    Text(g, L"—", w - Px(110), y + Px(23), Px(90), Px(18), 11, Muted(), true);
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
    if (g_menuOpen)
        return;
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
    if (g_editor)
        return;
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
    if (g_menuOpen)
        return;
    g_menuOpen = true;
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
    g_menuOpen = false;
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
#include "settings.hpp"

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {

        Placement();
        Render(g_overlay);
        return 0;
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return (hwnd == g_popup) ? MA_ACTIVATE : MA_NOACTIVATE;
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
            if (w == 3) {
                if (GetTickCount64() - g_rotatedAt >= 260) {
                    g_currentCoin = (g_currentCoin + 1) % Config().coins.size();
                    g_animating = false;
                    g_rotatedAt = GetTickCount64();
                    KillTimer(hwnd, 3);
                }
                Render(hwnd);
                return 0;
            }
            Placement();
            POINT cursor{};
            GetCursorPos(&cursor);
            if (!g_rotatedAt)
                g_rotatedAt = GetTickCount64();
            if (!g_animating && !g_menuOpen && !g_editor && !IsWindowVisible(g_popup) &&
                !Contains(g_overlay, cursor) && Config().coins.size() > 1 &&
                GetTickCount64() - g_rotatedAt >=
                    static_cast<ULONGLONG>(Config().rotationSeconds) * 1000) {
                g_rotatedAt = GetTickCount64();
                g_animating = true;
                SetTimer(hwnd, 3, 16, nullptr);
            }
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
    case WM_CONTEXTMENU:
        Menu();
        return 0;
    case WM_NCHITTEST:
        if (hwnd == g_overlay)
            return HTCLIENT;
        break;
    case WM_MOUSEWHEEL:
        if (hwnd == g_overlay) {
            const auto count = Config().coins.size();
            g_currentCoin =
                (g_currentCoin + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? count - 1 : 1)) % count;
            g_animating = false;
            g_rotatedAt = GetTickCount64();
            KillTimer(hwnd, 3);
            Render(hwnd);
        }
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
    case kShowList: {
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
    }
    LocalFree(argv);
    g_singleton = CreateMutexW(
        nullptr, FALSE, g_preview ? L"Local\\CryptoTaskbar.Preview" : L"Local\\CryptoTaskbar.v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND previous = FindWindowW(kClass, L"Crypto Taskbar");
        if (previous)
            PostMessageW(previous, kShowList, 0, WM_LBUTTONUP);
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
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls))
        return 1;
    g_overlay = CreateWindowExW(WS_EX_LAYERED | (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE), kClass,
                                L"Crypto Taskbar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance,
                                nullptr);
    g_popup =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClass, L"Crypto Taskbar — 自选行情",
                        WS_POPUP, 0, 0, 1, 1, g_overlay, nullptr, instance, nullptr);
    if (!g_overlay || !g_popup)
        return 1;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_dark = DarkTheme();
    Placement();
    Render(g_overlay);

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
