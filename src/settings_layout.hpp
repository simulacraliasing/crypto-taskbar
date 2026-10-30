void OpenEditor() {
    if (g_editor) {
        SetWindowPos(g_editor, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_editor);
        return;
    }
    g_draft = Config();
    g_controls.clear();
    g_fieldBoxes.clear();
    g_loading = true;
    WNDCLASSW cls{};
    cls.lpfnWndProc = EditorProc;
    cls.hInstance = g_instance;
    cls.lpszClassName = L"CryptoTaskbar.Settings.v3";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromRect(&g_barRect, MONITOR_DEFAULTTOPRIMARY), &monitor);
    g_editorScale = std::min(
        g_scale, static_cast<float>(monitor.rcWork.bottom - monitor.rcWork.top - 64) / 666.f);
    g_editorScale = std::min(
        g_editorScale, static_cast<float>(monitor.rcWork.right - monitor.rcWork.left - 32) / 820.f);
    RECT r{0, 0, E(820), E(666)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForWindow(g_overlay));
    g_editor = CreateWindowExW(
        0, cls.lpszClassName, L"Crypto Taskbar — 配置", style,
        monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - (r.right - r.left)) / 2,
        monitor.rcWork.top +
            std::max<LONG>(0,
                           (monitor.rcWork.bottom - monitor.rcWork.top - (r.bottom - r.top)) / 2),
        r.right - r.left, r.bottom - r.top, g_overlay, nullptr, g_instance, nullptr);
    if (!g_editor) {
        g_loading = false;
        return;
    }
    g_editFont = CreateFontW(-E(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    g_labelFont = CreateFontW(-E(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    Field(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_OWNERDRAWFIXED | TCS_BUTTONS | TCS_FLATBUTTONS,
          IdTabs, -1, 24, 18, 772, 44);
    SetWindowTheme(Ctrl(IdTabs), L"", L"");
    TabCtrl_SetItemSize(Ctrl(IdTabs), E(112), E(38));
    for (const auto *name : {L"自选币种", L"数据源", L"显示"}) {
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = const_cast<wchar_t *>(name);
        TabCtrl_InsertItem(Ctrl(IdTabs), TabCtrl_GetItemCount(Ctrl(IdTabs)), &tab);
    }
    const DWORD listStyle = WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS;
    Field(L"LISTBOX", L"", listStyle, IdCoinList, 0, 24, 86, 260, 450);
    Label(L"显示名称", 0, 320, 88);
    Edit(IdLabel, 0, 320, 112, 468);
    Label(L"数据源", 0, 320, 180);
    Choices(IdCoinSource, 0, 320, 204, 468, {});
    Label(L"交易对", 0, 320, 266);
    Edit(IdSymbol, 0, 320, 290, 468);
    Field(L"STATIC", L"", 0, IdHint, 0, 320, 340, 468, 22);
    Button(L"添加", IdCoinAdd, 0, 320, 394, 104);
    Button(L"更新", IdCoinUpdate, 0, 436, 394, 104);
    Button(L"删除", IdCoinDelete, 0, 24, 556, 80);
    Button(L"上移", IdUp, 0, 116, 556, 80);
    Button(L"下移", IdDown, 0, 208, 556, 76);
    Field(L"LISTBOX", L"", listStyle, IdSourceList, 1, 24, 86, 204, 450);
    Label(L"名称", 1, 260, 80, 250);
    Edit(IdSourceName, 1, 260, 104, 250);
    Label(L"接口代码格式", 1, 540, 80, 248);
    Choices(IdSymbolStyle, 1, 540, 104, 248, {L"BTCUSDT", L"BTC-USDT"});
    Label(L"接口 URL", 1, 260, 166);
    Edit(IdUrl, 1, 260, 190, 528);
    Label(L"价格路径", 1, 260, 250, 250);
    Edit(IdPricePath, 1, 260, 274, 250);
    Label(L"涨跌幅路径", 1, 540, 250, 248);
    Edit(IdChangePath, 1, 540, 274, 248);
    Label(L"开盘价路径（24h，可选）", 1, 260, 334, 250);
    Edit(IdOpenPath, 1, 260, 358, 250);
    Label(L"历史价格路径（可选）", 1, 540, 334, 248);
    Edit(IdHistoryPath, 1, 540, 358, 248);
    Label(L"历史接口 URL（其他周期，可选）", 1, 260, 418, 528);
    Edit(IdHistoryUrl, 1, 260, 442, 528);
    Label(L"请求头（每行 名称: 值）", 1, 260, 502, 250);
    Edit(IdHeaders, 1, 260, 526, 250, 68, true);
    Label(L"代码映射（BTCUSDT=bitcoin）", 1, 540, 502, 248);
    Edit(IdSymbolMap, 1, 540, 526, 248, 68, true);
    Button(L"删除", IdSourceDelete, 1, 24, 556, 60);
    Button(L"添加", IdSourceAdd, 1, 96, 556, 60);
    Button(L"更新", IdSourceUpdate, 1, 168, 556, 60);
    Label(L"行情刷新（秒）", 2, 32, 94, 220);
    Edit(IdRefreshSeconds, 2, 32, 120, 220);
    Label(L"轮播间隔（秒）", 2, 288, 94, 220);
    Edit(IdRotationSeconds, 2, 288, 120, 220);
    Label(L"涨跌幅周期", 2, 544, 94, 244);
    Choices(IdChangePeriod, 2, 544, 120, 244,
            {L"1 小时", L"4 小时", L"24 小时", L"7 天", L"30 天"});
    Label(L"显示宽度", 2, 32, 204, 220);
    Edit(IdWidth, 2, 32, 230, 220);
    Label(L"边缘距离", 2, 288, 204, 220);
    Edit(IdOffset, 2, 288, 230, 220);
    Label(L"价格前缀", 2, 544, 204, 244);
    Edit(IdCurrency, 2, 544, 230, 244);
    Label(L"显示位置", 2, 32, 314, 220);
    Choices(IdPosition, 2, 32, 340, 220, {L"任务栏左侧", L"系统托盘左侧"});
    Label(L"主题", 2, 288, 314, 220);
    Choices(IdTheme, 2, 288, 340, 220, {L"跟随系统", L"浅色", L"深色"});
    Field(L"BUTTON", L"任务栏显示涨跌幅", WS_TABSTOP | BS_AUTOCHECKBOX, IdShowChange, 2, 544, 342,
          244, 30);
    Label(L"上涨颜色", 2, 32, 426, 220);
    Button(L"", IdUpColor, 2, 32, 452, 220);
    Label(L"下跌颜色", 2, 288, 426, 220);
    Button(L"", IdDownColor, 2, 288, 452, 220);
    Button(L"保存", IdSave, -1, 568, 610, 104);
    Button(L"取消", IdCancel, -1, 684, 610, 104);
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
    const std::vector<std::string> periods{"1h", "4h", "24h", "7d", "30d"};
    SendMessageW(Ctrl(IdChangePeriod), CB_SETCURSEL,
                 std::find(periods.begin(), periods.end(), g_draft.changePeriod) - periods.begin(),
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
