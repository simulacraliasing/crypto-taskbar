// Included inside the application's private namespace, after its UI helpers.
enum ControlId {
    IdTabs = 200,
    IdCoinList,
    IdLabel,
    IdSymbol,
    IdCoinSource,
    IdCoinAdd,
    IdCoinUpdate,
    IdCoinDelete,
    IdUp,
    IdDown,
    IdSourceList,
    IdSourceName,
    IdUrl,
    IdPricePath,
    IdChangePath,
    IdOpenPath,
    IdHeaders,
    IdSourceAdd,
    IdSourceUpdate,
    IdSourceDelete,
    IdRefreshSeconds,
    IdRotationSeconds,
    IdWidth,
    IdOffset,
    IdPosition,
    IdTheme,
    IdCurrency,
    IdShowChange,
    IdSave,
    IdCancel,
    IdHint,
    IdChangePeriod,
    IdUpColor,
    IdDownColor,
    IdHistoryUrl,
    IdHistoryPath,
    IdSymbolStyle,
    IdSymbolMap
};
ct::Config g_draft;
std::vector<std::pair<HWND, int>> g_controls;
std::vector<std::string> g_sourceNames;
bool g_loading = false;
HFONT g_labelFont{};
float g_editorScale = 1;
struct FieldBox {
    RECT rect;
    int tab;
    HWND child;
};
std::vector<FieldBox> g_fieldBoxes;
int E(int n) { return static_cast<int>(n * g_editorScale + .5f); }
HWND Ctrl(int id) { return GetDlgItem(g_editor, id); }
std::string Value(int id) {
    int n = GetWindowTextLengthW(Ctrl(id));
    std::wstring s(n + 1, 0);
    GetWindowTextW(Ctrl(id), s.data(), n + 1);
    s.resize(n);
    return ct::Utf8(s);
}
void Put(int id, const std::string &text) { SetWindowTextW(Ctrl(id), ct::Wide(text).c_str()); }
int Selected(int id) { return static_cast<int>(SendMessageW(Ctrl(id), LB_GETCURSEL, 0, 0)); }
int Combo(int id) { return static_cast<int>(SendMessageW(Ctrl(id), CB_GETCURSEL, 0, 0)); }
HWND Field(const wchar_t *type, const wchar_t *text, DWORD style, int id, int tab, int x, int y,
           int w, int h) {
    HWND hwnd = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, E(x), E(y), E(w),
                                E(h), g_editor, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                g_instance, nullptr);
    SendMessageW(hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(wcscmp(type, L"STATIC") == 0 ? g_labelFont : g_editFont),
                 TRUE);
    if (wcscmp(type, L"COMBOBOX") == 0)
        SetWindowTheme(hwnd, L"Explorer", nullptr);
    g_controls.emplace_back(hwnd, tab);
    return hwnd;
}
void Label(const wchar_t *text, int tab, int x, int y, int w = 440) {
    Field(L"STATIC", text, 0, -1, tab, x, y, w, 21);
}
void Edit(int id, int tab, int x, int y, int w = 440, int h = 40, bool multiline = false) {
    HWND child = Field(
        L"EDIT", L"",
        WS_TABSTOP | (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL), id,
        tab, x + 10, y + 10, w - 20, h - 18);
    g_fieldBoxes.push_back({{E(x), E(y), E(x + w), E(y + h)}, tab, child});
    SendMessageW(Ctrl(id), EM_SETLIMITTEXT, multiline ? 16384 : 8192, 0);
}
void Button(const wchar_t *text, int id, int tab, int x, int y, int w = 84) {
    Field(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, id, tab, x, y, w, 36);
}
void Choices(int id, int tab, int x, int y, int w, const std::vector<std::wstring> &items) {
    Field(L"COMBOBOX", L"",
          WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, id, tab,
          x, y, w, 240);
    SendMessageW(Ctrl(id), CB_SETITEMHEIGHT, -1, E(30));
    SendMessageW(Ctrl(id), CB_SETITEMHEIGHT, 0, E(30));
    for (const auto &s : items)
        SendMessageW(Ctrl(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
}
void TabVisibility() {
    int tab = TabCtrl_GetCurSel(Ctrl(IdTabs));
    for (const auto &[hwnd, page] : g_controls)
        ShowWindow(hwnd, (page < 0 || page == tab) ? SW_SHOW : SW_HIDE);
    RedrawWindow(g_editor, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void RebuildSources() {
    g_sourceNames.clear();
    SendMessageW(Ctrl(IdSourceList), LB_RESETCONTENT, 0, 0);
    SendMessageW(Ctrl(IdCoinSource), CB_RESETCONTENT, 0, 0);
    for (const auto &[name, s] : g_draft.sources) {
        (void)s;
        g_sourceNames.push_back(name);
        const auto w = ct::Wide(name);
        SendMessageW(Ctrl(IdSourceList), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        SendMessageW(Ctrl(IdCoinSource), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
}
void RebuildCoins(int selected) {
    SendMessageW(Ctrl(IdCoinList), LB_RESETCONTENT, 0, 0);
    for (const auto &coin : g_draft.coins) {
        const auto s = ct::Wide(coin.label + "    " + coin.symbol + "    " + coin.source);
        SendMessageW(Ctrl(IdCoinList), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    }
    SendMessageW(Ctrl(IdCoinList), LB_SETCURSEL,
                 std::clamp(selected, 0, static_cast<int>(g_draft.coins.size()) - 1), 0);
}
void CoinHint() {
    std::wstring hint = L"例如 BTCUSDT、ETHUSDT、SOLUSDT";
    SetWindowTextW(Ctrl(IdHint), hint.c_str());
}
void LoadCoin() {
    int i = Selected(IdCoinList);
    if (i < 0 || i >= static_cast<int>(g_draft.coins.size()))
        return;
    g_loading = true;
    const auto &coin = g_draft.coins[i];
    Put(IdLabel, coin.label);
    Put(IdSymbol, coin.symbol);
    auto it = std::find(g_sourceNames.begin(), g_sourceNames.end(), coin.source);
    SendMessageW(Ctrl(IdCoinSource), CB_SETCURSEL, it - g_sourceNames.begin(), 0);
    CoinHint();
    g_loading = false;
}
void LoadSource() {
    int i = Selected(IdSourceList);
    if (i < 0 || i >= static_cast<int>(g_sourceNames.size()))
        return;
    g_loading = true;
    const auto &name = g_sourceNames[i];
    const auto &s = g_draft.sources.at(name);
    Put(IdSourceName, name);
    Put(IdUrl, s.url);
    Put(IdPricePath, s.pricePath);
    Put(IdChangePath, s.changePath);
    Put(IdOpenPath, s.openPath);
    Put(IdHistoryUrl, s.historyUrl);
    Put(IdHistoryPath, s.historyPricePath);
    SendMessageW(Ctrl(IdSymbolStyle), CB_SETCURSEL, s.symbolStyle == "dashed" ? 1 : 0, 0);
    std::string mappings;
    for (const auto &[pair, id] : s.symbolMap)
        mappings += pair + "=" + id + "\r\n";
    Put(IdSymbolMap, mappings);
    std::string headers;
    for (const auto &[key, value] : s.headers)
        headers += key + ": " + value + "\r\n";
    Put(IdHeaders, headers);
    g_loading = false;
}
int Integer(int id) {
    const auto text = Value(id);
    size_t end = 0;
    int n = std::stoi(text, &end);
    ct::Require(end == text.size(), "请输入整数。");
    return n;
}
void ReadDisplay(ct::Config &c) {
    c.refreshSeconds = Integer(IdRefreshSeconds);
    c.rotationSeconds = Integer(IdRotationSeconds);
    c.width = Integer(IdWidth);
    c.offset = Integer(IdOffset);
    c.position = Combo(IdPosition) == 1 ? "right" : "left";
    const int theme = Combo(IdTheme);
    c.theme = theme == 1 ? "light" : theme == 2 ? "dark" : "system";
    c.currency = Value(IdCurrency);
    c.showChange = SendMessageW(Ctrl(IdShowChange), BM_GETCHECK, 0, 0) == BST_CHECKED;
    static const std::vector<std::string> periods{"1h", "4h", "24h", "7d", "30d"};
    const int p = Combo(IdChangePeriod);
    c.changePeriod = periods.at(p >= 0 ? p : 2);
}
void UpdateCoin(bool add) {
    auto copy = g_draft;
    const int source = Combo(IdCoinSource);
    ct::Require(source >= 0 && source < static_cast<int>(g_sourceNames.size()), "请选择数据源。");
    ct::Coin coin{Value(IdLabel), Value(IdSymbol), g_sourceNames[source]};
    int index = Selected(IdCoinList);
    if (add) {
        copy.coins.push_back(coin);
        index = static_cast<int>(copy.coins.size()) - 1;
    } else {
        ct::Require(index >= 0, "请先选择币种。");
        copy.coins[index] = coin;
    }
    copy = ct::ParseConfig(ct::SerializeConfig(copy));
    g_draft = std::move(copy);
    RebuildCoins(index);
    LoadCoin();
}
std::string Trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
void UpdateSource(bool add) {
    const int index = Selected(IdSourceList);
    auto copy = g_draft;
    const auto name = Trim(Value(IdSourceName));
    ct::Require(!name.empty(), "请填写数据源名称。");
    std::string old;
    if (!add) {
        ct::Require(index >= 0, "请先选择数据源。");
        old = g_sourceNames[index];
    }
    ct::Require((!add && name == old) || !copy.sources.count(name), "该数据源名称已存在。");
    ct::Source s{Value(IdUrl), Value(IdPricePath), Value(IdChangePath), Value(IdOpenPath), {}};
    s.historyUrl = Value(IdHistoryUrl);
    s.historyPricePath = Value(IdHistoryPath);
    s.symbolStyle = Combo(IdSymbolStyle) == 1 ? "dashed" : "plain";
    std::istringstream mapLines(Value(IdSymbolMap));
    std::string mapping;
    while (std::getline(mapLines, mapping)) {
        mapping = Trim(mapping);
        if (mapping.empty())
            continue;
        auto p = mapping.find('=');
        ct::Require(p != std::string::npos, "代码映射格式：BTCUSDT=bitcoin");
        s.symbolMap[ct::CanonicalPair(Trim(mapping.substr(0, p)))] = Trim(mapping.substr(p + 1));
    }
    std::istringstream lines(Value(IdHeaders));
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty())
            continue;
        auto p = line.find(':');
        ct::Require(p != std::string::npos, "请求头格式：名称: 值，每行一项。");
        s.headers[Trim(line.substr(0, p))] = Trim(line.substr(p + 1));
    }
    if (!old.empty() && old != name) {
        copy.sources.erase(old);
        for (auto &coin : copy.coins)
            if (coin.source == old)
                coin.source = name;
    }
    copy.sources[name] = std::move(s);
    copy = ct::ParseConfig(ct::SerializeConfig(copy));
    g_draft = std::move(copy);
    RebuildSources();
    SendMessageW(
        Ctrl(IdSourceList), LB_SETCURSEL,
        std::find(g_sourceNames.begin(), g_sourceNames.end(), name) - g_sourceNames.begin(), 0);
    RebuildCoins(0);
    LoadCoin();
    LoadSource();
}
void EditorTheme() {
    if (!g_editor)
        return;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_editor, 20, &dark, sizeof(dark));
    RedrawWindow(g_editor, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
#include "settings_style.hpp"

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ERASEBKGND: {
        DrawSettingsBackground(reinterpret_cast<HDC>(w));
        return 1;
    }
    case WM_DRAWITEM:
        DrawSettingsItem(reinterpret_cast<DRAWITEMSTRUCT *>(l));
        return TRUE;
    case WM_MEASUREITEM: {
        auto *item = reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        item->itemHeight =
            E(item->CtlType == ODT_LISTBOX ? (item->CtlID == IdCoinList ? 58 : 44) : 32);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(w);
        const auto bg = msg == WM_CTLCOLORSTATIC ? UiBackground() : UiSurface();
        SetTextColor(dc, g_dark ? RGB(240, 242, 246) : RGB(24, 29, 37));
        SetBkColor(dc, bg);
        SetDCBrushColor(dc, bg);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_NOTIFY:
        if (!g_loading && reinterpret_cast<NMHDR *>(l)->idFrom == IdTabs &&
            reinterpret_cast<NMHDR *>(l)->code == TCN_SELCHANGING) {
            try {
                int page = TabCtrl_GetCurSel(Ctrl(IdTabs));
                if (page == 0)
                    UpdateCoin(false);
                if (page == 1)
                    UpdateSource(false);
            } catch (const std::exception &e) {
                MessageBoxW(hwnd, ct::Wide(e.what()).c_str(), L"请检查配置",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
        }
        if (reinterpret_cast<NMHDR *>(l)->idFrom == IdTabs &&
            reinterpret_cast<NMHDR *>(l)->code == TCN_SELCHANGE) {
            TabVisibility();
            return 0;
        }
        break;
    case WM_COMMAND: {
        if (g_loading)
            return 0;
        const int id = LOWORD(w), code = HIWORD(w);
        if (id == IdCoinList && code == LBN_SELCHANGE) {
            LoadCoin();
            return 0;
        }
        if (id == IdSourceList && code == LBN_SELCHANGE) {
            LoadSource();
            return 0;
        }
        if (id == IdCoinSource && code == CBN_SELCHANGE) {
            CoinHint();
            return 0;
        }
        if (code != BN_CLICKED)
            return 0;
        try {
            if (id == IdUpColor || id == IdDownColor)
                ChooseSettingColor(id);
            else if (id == IdCoinAdd)
                UpdateCoin(true);
            else if (id == IdCoinUpdate)
                UpdateCoin(false);
            else if (id == IdCoinDelete) {
                int i = Selected(IdCoinList);
                ct::Require(i >= 0 && g_draft.coins.size() > 1, "至少保留一个币种。");
                g_draft.coins.erase(g_draft.coins.begin() + i);
                RebuildCoins(i);
                LoadCoin();
            } else if (id == IdUp || id == IdDown) {
                int i = Selected(IdCoinList), j = i + (id == IdUp ? -1 : 1);
                if (i >= 0 && j >= 0 && j < static_cast<int>(g_draft.coins.size())) {
                    std::swap(g_draft.coins[i], g_draft.coins[j]);
                    RebuildCoins(j);
                    LoadCoin();
                }
            } else if (id == IdSourceAdd)
                UpdateSource(true);
            else if (id == IdSourceUpdate)
                UpdateSource(false);
            else if (id == IdSourceDelete) {
                int i = Selected(IdSourceList);
                ct::Require(i >= 0, "请先选择数据源。");
                const auto name = g_sourceNames[i];
                for (const auto &coin : g_draft.coins)
                    ct::Require(coin.source != name, "还有币种使用这个数据源，请先更换。");
                g_draft.sources.erase(name);
                RebuildSources();
                SendMessageW(Ctrl(IdSourceList), LB_SETCURSEL, 0, 0);
                LoadSource();
                LoadCoin();
            } else if (id == IdSave) {
                int page = TabCtrl_GetCurSel(Ctrl(IdTabs));
                if (page == 0)
                    UpdateCoin(false);
                if (page == 1)
                    UpdateSource(false);
                auto c = g_draft;
                ReadDisplay(c);
                c = ct::ParseConfig(ct::SerializeConfig(c));
                SaveText(ct::SerializeConfig(c));
                Apply(std::move(c));
                DestroyWindow(hwnd);
            } else if (id == IdCancel || id == IDCANCEL)
                DestroyWindow(hwnd);
        } catch (const std::exception &e) {
            MessageBoxW(hwnd, ct::Wide(e.what()).c_str(), L"请检查配置", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_editor = nullptr;
        g_controls.clear();
        g_fieldBoxes.clear();
        if (g_editFont) {
            DeleteObject(g_editFont);
            g_editFont = nullptr;
        }
        if (g_labelFont) {
            DeleteObject(g_labelFont);
            g_labelFont = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
#include "settings_layout.hpp"
