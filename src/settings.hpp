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
    IdHint
};
ct::Config g_draft;
std::vector<std::pair<HWND, int>> g_controls;
std::vector<std::string> g_sourceNames;
bool g_loading = false;
int E(int n) { return MulDiv(n, static_cast<int>(GetDpiForWindow(g_editor)), 96); }
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
    HWND hwnd =
        CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, text,
                        WS_CHILD | WS_VISIBLE | style, E(x), E(y), E(w), E(h), g_editor,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    g_controls.emplace_back(hwnd, tab);
    return hwnd;
}
void Label(const wchar_t *text, int tab, int x, int y, int w = 440) {
    Field(L"STATIC", text, 0, -1, tab, x, y, w, 21);
}
void Edit(int id, int tab, int x, int y, int w = 440, int h = 27, bool multiline = false) {
    Field(L"EDIT", L"",
          WS_TABSTOP | (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL),
          id, tab, x, y, w, h);
    SendMessageW(Ctrl(id), EM_SETLIMITTEXT, multiline ? 16384 : 8192, 0);
}
void Button(const wchar_t *text, int id, int tab, int x, int y, int w = 84) {
    Field(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, id, tab, x, y, w, 29);
}
void Choices(int id, int tab, int x, int y, int w, const std::vector<std::wstring> &items) {
    Field(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id, tab, x, y, w, 200);
    for (const auto &s : items)
        SendMessageW(Ctrl(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
}
void TabVisibility() {
    int tab = TabCtrl_GetCurSel(Ctrl(IdTabs));
    for (const auto &[hwnd, page] : g_controls)
        ShowWindow(hwnd, (page < 0 || page == tab) ? SW_SHOW : SW_HIDE);
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
    const int i = Combo(IdCoinSource);
    std::wstring hint = L"接口使用的币种代码";
    if (i >= 0 && i < static_cast<int>(g_sourceNames.size())) {
        const auto &name = g_sourceNames[i];
        if (name == "coingecko")
            hint = L"例如 bitcoin、ethereum、solana";
        else if (name == "binance")
            hint = L"例如 BTCUSDT、ETHUSDT、SOLUSDT";
        else if (name == "okx")
            hint = L"例如 BTC-USDT、ETH-USDT、SOL-USDT";
    }
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
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(w);
        const auto bg = g_dark ? RGB(31, 34, 40) : RGB(249, 250, 252);
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
            if (id == IdCoinAdd)
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
        SetWindowPos(g_editor, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_editor);
        return;
    }
    g_draft = Config();
    g_controls.clear();
    g_loading = true;
    WNDCLASSW cls{};
    cls.lpfnWndProc = EditorProc;
    cls.hInstance = g_instance;
    cls.lpszClassName = L"CryptoTaskbar.Settings.v2";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, Px(760), Px(574)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    g_editor = CreateWindowExW(0, cls.lpszClassName, L"Crypto Taskbar — 配置", style, CW_USEDEFAULT,
                               CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, g_overlay,
                               nullptr, g_instance, nullptr);
    if (!g_editor) {
        g_loading = false;
        return;
    }
    g_editFont = CreateFontW(-E(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    Field(WC_TABCONTROLW, L"", WS_TABSTOP, IdTabs, -1, 16, 12, 728, 32);
    for (const auto *name : {L"币种", L"数据源", L"显示"}) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t *>(name);
        TabCtrl_InsertItem(Ctrl(IdTabs), TabCtrl_GetItemCount(Ctrl(IdTabs)), &item);
    }
    Field(L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
          IdCoinList, 0, 20, 62, 252, 424);
    Label(L"显示名称", 0, 300, 66);
    Edit(IdLabel, 0, 300, 90, 420);
    Label(L"数据源", 0, 300, 132);
    Choices(IdCoinSource, 0, 300, 156, 420, {});
    Label(L"币种代码", 0, 300, 198);
    Edit(IdSymbol, 0, 300, 222, 420);
    Field(L"STATIC", L"", 0, IdHint, 0, 300, 254, 420, 22);
    Button(L"添加", IdCoinAdd, 0, 300, 302);
    Button(L"更新", IdCoinUpdate, 0, 396, 302);
    Button(L"删除", IdCoinDelete, 0, 20, 496, 76);
    Button(L"上移", IdUp, 0, 104, 496, 76);
    Button(L"下移", IdDown, 0, 188, 496, 76);
    Field(L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
          IdSourceList, 1, 20, 62, 220, 424);
    Label(L"名称", 1, 264, 60);
    Edit(IdSourceName, 1, 264, 82, 456);
    Label(L"接口 URL", 1, 264, 116);
    Edit(IdUrl, 1, 264, 138, 456);
    Label(L"价格路径", 1, 264, 172);
    Edit(IdPricePath, 1, 264, 194, 456);
    Label(L"24 小时涨跌幅路径（可选）", 1, 264, 228);
    Edit(IdChangePath, 1, 264, 250, 456);
    Label(L"24 小时开盘价路径（可选）", 1, 264, 284);
    Edit(IdOpenPath, 1, 264, 306, 456);
    Label(L"请求头（每行 名称: 值，可使用 %环境变量%）", 1, 264, 340);
    Edit(IdHeaders, 1, 264, 362, 456, 88, true);
    Button(L"添加", IdSourceAdd, 1, 264, 468);
    Button(L"更新", IdSourceUpdate, 1, 360, 468);
    Button(L"删除", IdSourceDelete, 1, 20, 496);
    const std::vector<std::pair<const wchar_t *, int>> fields{
        {L"行情刷新（秒）", IdRefreshSeconds},
        {L"轮播间隔（秒）", IdRotationSeconds},
        {L"显示宽度", IdWidth},
        {L"边缘距离", IdOffset},
        {L"价格前缀", IdCurrency}};
    int y = 74;
    for (const auto &[name, id] : fields) {
        Label(name, 2, 32, y, 200);
        Edit(id, 2, 240, y - 3, 180);
        y += 50;
    }
    Label(L"显示位置", 2, 32, 324, 200);
    Choices(IdPosition, 2, 240, 321, 180, {L"任务栏左侧", L"系统托盘左侧"});
    Label(L"主题", 2, 32, 374, 200);
    Choices(IdTheme, 2, 240, 371, 180, {L"跟随系统", L"浅色", L"深色"});
    Field(L"BUTTON", L"任务栏同时显示涨跌幅", WS_TABSTOP | BS_AUTOCHECKBOX, IdShowChange, 2, 32,
          430, 330, 28);
    Button(L"保存", IdSave, -1, 548, 534);
    Button(L"取消", IdCancel, -1, 644, 534);
    RebuildSources();
    RebuildCoins(0);
    LoadCoin();
    SendMessageW(Ctrl(IdSourceList), LB_SETCURSEL, 0, 0);
    LoadSource();
    Put(IdRefreshSeconds, std::to_string(g_draft.refreshSeconds));
    Put(IdRotationSeconds, std::to_string(g_draft.rotationSeconds));
    Put(IdWidth, std::to_string(g_draft.width));
    Put(IdOffset, std::to_string(g_draft.offset));
    Put(IdCurrency, g_draft.currency);
    SendMessageW(Ctrl(IdPosition), CB_SETCURSEL, g_draft.position == "right" ? 1 : 0, 0);
    SendMessageW(Ctrl(IdTheme), CB_SETCURSEL,
                 g_draft.theme == "light"  ? 1
                 : g_draft.theme == "dark" ? 2
                                           : 0,
                 0);
    SendMessageW(Ctrl(IdShowChange), BM_SETCHECK, g_draft.showChange ? BST_CHECKED : BST_UNCHECKED,
                 0);
    TabVisibility();
    g_loading = false;
    EditorTheme();
    SetWindowPos(g_editor, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_editor);
    SetFocus(Ctrl(IdCoinList));
}
