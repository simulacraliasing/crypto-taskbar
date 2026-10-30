void UiLabelText(Graphics &g, const std::wstring &text, double x, double y, double w, double h,
                 double size, Color color, bool right = false, bool bold = false) {
    Text(g, text, x, y, w, h, size, color, right, bold, g_editorScale);
}
COLORREF UiBackground() { return g_dark ? RGB(23, 26, 32) : RGB(245, 247, 250); }
COLORREF UiSurface() { return g_dark ? RGB(33, 37, 45) : RGB(255, 255, 255); }
Color UiColor(COLORREF c) { return Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }
Color UiText() { return g_dark ? Color(255, 236, 240, 247) : Color(255, 31, 41, 57); }
Color UiMuted() { return g_dark ? Color(255, 156, 168, 187) : Color(255, 104, 116, 136); }
Color HexColor(const std::string &text) {
    const auto v = std::stoul(text.substr(1), nullptr, 16);
    return Color(255, static_cast<BYTE>(v >> 16), static_cast<BYTE>(v >> 8), static_cast<BYTE>(v));
}
void DrawSettingsBackground(HDC dc) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(UiColor(UiBackground()));
    const int page = TabCtrl_GetCurSel(Ctrl(IdTabs));
    for (const auto &field : g_fieldBoxes)
        if (field.tab == page) {
            const auto &r = field.rect;
            const RectF rect(static_cast<float>(r.left), static_cast<float>(r.top),
                             static_cast<float>(r.right - r.left),
                             static_cast<float>(r.bottom - r.top));
            RoundRect(g, rect, E(7),
                      GetFocus() == field.child
                          ? Color(255, 86, 134, 224)
                          : (g_dark ? Color(255, 53, 61, 75) : Color(255, 219, 225, 235)));
            RoundRect(g, RectF(rect.X + 1, rect.Y + 1, rect.Width - 2, rect.Height - 2), E(6),
                      UiColor(UiSurface()));
        }
}
void DrawSettingsItem(DRAWITEMSTRUCT *item) {
    if (!item)
        return;
    Graphics g(item->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const RECT &r = item->rcItem;
    const float x = static_cast<float>(r.left), y = static_cast<float>(r.top),
                w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    SolidBrush background(UiColor(UiBackground()));
    g.FillRectangle(&background, x, y, w, h);
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    if (item->CtlType == ODT_TAB) {
        wchar_t text[64]{};
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = text;
        tab.cchTextMax = 64;
        TabCtrl_GetItem(Ctrl(IdTabs), item->itemID, &tab);
        UiLabelText(g, text, x + E(12), y, w - E(24), h - 3, 14,
                    selected ? Color(255, 52, 108, 207) : UiMuted(), false, selected);
        if (selected)
            RoundRect(g, RectF(x + E(12), y + h - E(3), w - E(24), static_cast<float>(E(3))), E(1),
                      Color(255, 65, 118, 218));
        return;
    }
    if (item->CtlType == ODT_LISTBOX) {
        SolidBrush surface(UiColor(UiSurface()));
        g.FillRectangle(&surface, x, y, w, h);
        if (item->itemID == static_cast<UINT>(-1))
            return;
        if (selected)
            RoundRect(g, RectF(x + E(6), y + E(3), w - E(12), h - E(6)), E(7),
                      g_dark ? Color(255, 45, 62, 87) : Color(255, 232, 240, 253));
        if (item->CtlID == IdCoinList && item->itemID < g_draft.coins.size()) {
            const auto &coin = g_draft.coins[item->itemID];
            UiLabelText(g, ct::Wide(coin.label), x + E(18), y + E(7), w - E(36), E(24), 15,
                        UiText(), false, true);
            UiLabelText(g, ct::Wide(coin.symbol), x + E(18), y + E(31), w - E(36), E(20), 11,
                        UiMuted());
        } else if (item->itemID < g_sourceNames.size())
            UiLabelText(g, ct::Wide(g_sourceNames[item->itemID]), x + E(16), y, w - E(32), h, 14,
                        UiText(), false, selected);
        return;
    }
    if (item->CtlType == ODT_COMBOBOX) {
        SolidBrush surface(selected ? Color(255, 224, 235, 253) : UiColor(UiSurface()));
        g.FillRectangle(&surface, x, y, w, h);
        int index = static_cast<int>(item->itemID);
        if (index < 0)
            index = static_cast<int>(SendMessageW(item->hwndItem, CB_GETCURSEL, 0, 0));
        wchar_t text[256]{};
        if (index >= 0)
            SendMessageW(item->hwndItem, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
        UiLabelText(g, text, x + E(10), y, w - E(20), h, 14, UiText());
        return;
    }
    if (item->CtlType == ODT_BUTTON) {
        const bool primary = item->CtlID == IdSave;
        Color border = primary ? Color(255, 56, 112, 213)
                               : (g_dark ? Color(255, 57, 66, 82) : Color(255, 218, 225, 235));
        Color fill = primary
                         ? (selected ? Color(255, 41, 91, 183) : Color(255, 56, 112, 213))
                         : (selected ? (g_dark ? Color(255, 47, 55, 69) : Color(255, 231, 236, 244))
                                     : UiColor(UiSurface()));
        RoundRect(g, RectF(x + 1, y + 1, w - 2, h - 2), E(7), border);
        RoundRect(g, RectF(x + 2, y + 2, w - 4, h - 4), E(6), fill);
        wchar_t text[128]{};
        GetWindowTextW(item->hwndItem, text, 128);
        if (item->CtlID == IdUpColor || item->CtlID == IdDownColor) {
            const auto &color = item->CtlID == IdUpColor ? g_draft.upColor : g_draft.downColor;
            RoundRect(g,
                      RectF(x + E(14), y + (h - E(16)) / 2, static_cast<float>(E(16)),
                            static_cast<float>(E(16))),
                      E(4), HexColor(color));
            UiLabelText(g, ct::Wide(color), x + E(42), y, w - E(52), h, 14, UiText());
        } else {
            FontFamily family(L"Segoe UI");
            Font font(&family, 14 * g_editorScale, primary ? FontStyleBold : FontStyleRegular,
                      UnitPixel);
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            SolidBrush brush(primary ? Color(255, 255, 255, 255) : UiText());
            g.DrawString(text, -1, &font, RectF(x, y, w, h), &format, &brush);
        }
    }
}
void ChooseSettingColor(int id) {
    auto &text = id == IdUpColor ? g_draft.upColor : g_draft.downColor;
    const auto color = HexColor(text);
    static COLORREF custom[16]{};
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = g_editor;
    choice.lpCustColors = custom;
    choice.rgbResult = RGB(color.GetR(), color.GetG(), color.GetB());
    choice.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&choice)) {
        char value[8]{};
        snprintf(value, sizeof(value), "#%02X%02X%02X", GetRValue(choice.rgbResult),
                 GetGValue(choice.rgbResult), GetBValue(choice.rgbResult));
        text = value;
        InvalidateRect(Ctrl(id), nullptr, TRUE);
    }
}
