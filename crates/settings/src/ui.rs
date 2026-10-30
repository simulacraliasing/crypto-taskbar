use anyhow::{Context as _, Result as AResult, ensure};
use crypto_taskbar_core::{Coin, Config, Source, canonical_pair, parse_color};
use gpui_kit::component::{
    button::*, checkbox::Checkbox, color_picker::*, input::*, list::ListItem, select::*, *,
};
use gpui_kit::*;
use std::path::PathBuf;

#[derive(Clone, Debug)]
struct Choice {
    title: SharedString,
    value: SharedString,
}
impl SelectItem for Choice {
    type Value = SharedString;
    fn title(&self) -> SharedString {
        self.title.clone()
    }
    fn value(&self) -> &Self::Value {
        &self.value
    }
}
type Picker = Entity<SelectState<Vec<Choice>>>;
fn input(value: impl Into<SharedString>, window: &mut Window, cx: &mut App) -> Entity<InputState> {
    let value = value.into();
    cx.new(|cx| InputState::new(window, cx).default_value(value))
}
fn textarea(value: String, window: &mut Window, cx: &mut App) -> Entity<TextareaState> {
    cx.new(|cx| TextareaState::new(window, cx).rows(3).default_value(value))
}
fn picker(
    options: Vec<(String, String)>,
    value: &str,
    window: &mut Window,
    cx: &mut App,
) -> Picker {
    let selected = options
        .iter()
        .position(|(_, key)| key == value)
        .unwrap_or(0);
    let options = options
        .into_iter()
        .map(|(title, value)| Choice {
            title: title.into(),
            value: value.into(),
        })
        .collect::<Vec<_>>();
    cx.new(|cx| {
        SelectState::new(
            options,
            Some(IndexPath::default().row(selected)),
            window,
            cx,
        )
    })
}
fn pick(state: &Picker, cx: &App) -> String {
    state
        .read(cx)
        .selected_value()
        .map(ToString::to_string)
        .unwrap_or_default()
}
fn value(state: &Entity<InputState>, cx: &App) -> String {
    state.read(cx).value().trim().to_owned()
}
fn color_value(state: &Entity<ColorPickerState>, cx: &App) -> String {
    let rgba: Rgba = state
        .read(cx)
        .value()
        .unwrap_or_else(|| rgb(0x808080).into())
        .into();
    format!(
        "#{:02X}{:02X}{:02X}",
        (rgba.r * 255.).round() as u8,
        (rgba.g * 255.).round() as u8,
        (rgba.b * 255.).round() as u8
    )
}
fn field(label: &'static str, control: impl IntoElement) -> impl IntoElement {
    v_flex()
        .w_full()
        .min_w_0()
        .gap_2()
        .child(div().text_sm().child(label))
        .child(control)
}
fn row(a: impl IntoElement, b: impl IntoElement) -> impl IntoElement {
    h_flex()
        .w_full()
        .items_start()
        .gap_5()
        .child(div().flex_1().min_w_0().child(a))
        .child(div().flex_1().min_w_0().child(b))
}
fn choices(items: &[(&str, &str)]) -> Vec<(String, String)> {
    items
        .iter()
        .map(|(a, b)| (a.to_string(), b.to_string()))
        .collect()
}

struct CoinForm {
    label: Entity<InputState>,
    symbol: Entity<InputState>,
    source: Picker,
}
impl CoinForm {
    fn new(coin: &Coin, c: &Config, window: &mut Window, cx: &mut App) -> Self {
        Self {
            label: input(coin.label.clone(), window, cx),
            symbol: input(coin.symbol.clone(), window, cx),
            source: picker(
                c.sources.keys().map(|s| (s.clone(), s.clone())).collect(),
                &coin.source,
                window,
                cx,
            ),
        }
    }
    fn read(&self, cx: &App) -> Coin {
        Coin {
            label: value(&self.label, cx),
            symbol: canonical_pair(&value(&self.symbol, cx)),
            source: pick(&self.source, cx),
        }
    }
}
struct SourceForm {
    name: Entity<InputState>,
    url: Entity<InputState>,
    price: Entity<InputState>,
    change: Entity<InputState>,
    open: Entity<InputState>,
    history: Entity<InputState>,
    history_price: Entity<InputState>,
    style: Picker,
    headers: Entity<TextareaState>,
    mapping: Entity<TextareaState>,
}
impl SourceForm {
    fn new(name: &str, s: &Source, window: &mut Window, cx: &mut App) -> Self {
        Self {
            name: input(name.to_string(), window, cx),
            url: input(s.url.clone(), window, cx),
            price: input(s.price_path.clone(), window, cx),
            change: input(s.change_path.clone(), window, cx),
            open: input(s.open_path.clone(), window, cx),
            history: input(s.history_url.clone(), window, cx),
            history_price: input(s.history_price_path.clone(), window, cx),
            style: picker(
                choices(&[("BTCUSDT", "plain"), ("BTC-USDT", "dashed")]),
                &s.symbol_style,
                window,
                cx,
            ),
            headers: textarea(
                s.headers
                    .iter()
                    .map(|(k, v)| format!("{k}: {v}"))
                    .collect::<Vec<_>>()
                    .join("\n"),
                window,
                cx,
            ),
            mapping: textarea(
                s.symbol_map
                    .iter()
                    .map(|(k, v)| format!("{k}={v}"))
                    .collect::<Vec<_>>()
                    .join("\n"),
                window,
                cx,
            ),
        }
    }
    fn read(&self, cx: &App) -> AResult<(String, Source)> {
        let mut s = Source {
            url: value(&self.url, cx),
            price_path: value(&self.price, cx),
            change_path: value(&self.change, cx),
            open_path: value(&self.open, cx),
            history_url: value(&self.history, cx),
            history_price_path: value(&self.history_price, cx),
            symbol_style: pick(&self.style, cx),
            ..Source::default()
        };
        for line in self
            .headers
            .read(cx)
            .value()
            .lines()
            .filter(|l| !l.trim().is_empty())
        {
            let (key, val) = line
                .split_once(':')
                .context("请求头格式：名称: 值，每行一项。")?;
            s.headers.insert(key.trim().into(), val.trim().into());
        }
        for line in self
            .mapping
            .read(cx)
            .value()
            .lines()
            .filter(|l| !l.trim().is_empty())
        {
            let (key, val) = line
                .split_once('=')
                .context("代码映射格式：BTCUSDT=bitcoin")?;
            s.symbol_map
                .insert(canonical_pair(key.trim()), val.trim().into());
        }
        Ok((value(&self.name, cx), s))
    }
}
struct DisplayForm {
    refresh: Entity<InputState>,
    rotation: Entity<InputState>,
    width: Entity<InputState>,
    offset: Entity<InputState>,
    currency: Entity<InputState>,
    position: Picker,
    theme: Picker,
    period: Picker,
    up: Entity<ColorPickerState>,
    down: Entity<ColorPickerState>,
    show_change: bool,
}
impl DisplayForm {
    fn new(c: &Config, window: &mut Window, cx: &mut App) -> Self {
        Self {
            refresh: input(c.refresh_seconds.to_string(), window, cx),
            rotation: input(c.rotation_seconds.to_string(), window, cx),
            width: input(c.width.to_string(), window, cx),
            offset: input(c.offset.to_string(), window, cx),
            currency: input(c.currency.clone(), window, cx),
            position: picker(
                choices(&[("任务栏左侧", "left"), ("系统托盘左侧", "right")]),
                &c.position,
                window,
                cx,
            ),
            theme: picker(
                choices(&[("跟随系统", "system"), ("浅色", "light"), ("深色", "dark")]),
                &c.theme,
                window,
                cx,
            ),
            period: picker(
                choices(&[
                    ("1 小时", "1h"),
                    ("4 小时", "4h"),
                    ("24 小时", "24h"),
                    ("7 天", "7d"),
                    ("30 天", "30d"),
                ]),
                &c.change_period,
                window,
                cx,
            ),
            up: cx.new(|cx| {
                ColorPickerState::new(window, cx)
                    .default_value(rgb(parse_color(&c.up_color).unwrap_or(0x159f76)))
            }),
            down: cx.new(|cx| {
                ColorPickerState::new(window, cx)
                    .default_value(rgb(parse_color(&c.down_color).unwrap_or(0xe05260)))
            }),
            show_change: c.show_change,
        }
    }
    fn read(&self, c: &mut Config, cx: &App) -> AResult<()> {
        c.refresh_seconds = value(&self.refresh, cx)
            .parse()
            .context("行情刷新需要填写秒数。")?;
        c.rotation_seconds = value(&self.rotation, cx)
            .parse()
            .context("轮播间隔需要填写秒数。")?;
        c.width = value(&self.width, cx)
            .parse()
            .context("显示宽度需要填写整数。")?;
        c.offset = value(&self.offset, cx)
            .parse()
            .context("边缘距离需要填写整数。")?;
        c.currency = value(&self.currency, cx);
        c.position = pick(&self.position, cx);
        c.theme = pick(&self.theme, cx);
        c.change_period = pick(&self.period, cx);
        c.up_color = color_value(&self.up, cx);
        c.down_color = color_value(&self.down, cx);
        c.show_change = self.show_change;
        Ok(())
    }
}

pub struct Settings {
    draft: Config,
    path: PathBuf,
    notify_class: String,
    page: usize,
    selected_coin: Option<usize>,
    selected_source: Option<String>,
    coin: CoinForm,
    source: SourceForm,
    display: DisplayForm,
    error: Option<String>,
    last_dark: std::cell::Cell<Option<bool>>,
    _theme_task: Task<()>,
    _subscriptions: Vec<Subscription>,
}
impl Settings {
    pub fn new(
        config: Config,
        path: PathBuf,
        notify_class: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let coin = CoinForm::new(&config.coins[0], &config, window, cx);
        let source_name = config.sources.keys().next().unwrap().clone();
        let source = SourceForm::new(&source_name, &config.sources[&source_name], window, cx);
        let display = DisplayForm::new(&config, window, cx);
        let subscriptions = vec![
            cx.subscribe_in(
                &display.theme,
                window,
                |this, _, _: &SelectEvent<Vec<Choice>>, window, cx| {
                    this.apply_theme(window, cx);
                    cx.notify();
                },
            ),
            cx.observe_window_appearance(window, |this, window, cx| {
                this.apply_theme(window, cx);
            }),
        ];
        // Windows can change the taskbar theme independently of app appearance.
        let theme_task = cx.spawn_in(window, async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(std::time::Duration::from_secs(1))
                    .await;
                if this
                    .update_in(cx, |this, window, cx| this.apply_theme(window, cx))
                    .is_err()
                {
                    break;
                }
            }
        });
        let this = Self {
            draft: config,
            path,
            notify_class,
            page: 0,
            selected_coin: Some(0),
            selected_source: Some(source_name),
            coin,
            source,
            display,
            error: None,
            last_dark: std::cell::Cell::new(None),
            _theme_task: theme_task,
            _subscriptions: subscriptions,
        };
        this.apply_theme(window, cx);
        this
    }
    fn apply_theme(&self, window: &mut Window, cx: &mut App) {
        let dark = match pick(&self.display.theme, cx).as_str() {
            "dark" => true,
            "light" => false,
            _ => crate::platform::system_dark(),
        };
        if self.last_dark.replace(Some(dark)) == Some(dark) {
            return;
        }
        crate::platform::titlebar_theme(window, dark);
        Theme::change(
            if dark {
                ThemeMode::Dark
            } else {
                ThemeMode::Light
            },
            Some(window),
            cx,
        );
    }
    fn commit_coin(&mut self, cx: &App) -> AResult<()> {
        let coin = self.coin.read(cx);
        if self.selected_coin.is_none() && coin.label.is_empty() && coin.symbol.is_empty() {
            return Ok(());
        }
        let mut draft = self.draft.clone();
        if let Some(index) = self.selected_coin {
            draft.coins[index] = coin;
        } else {
            draft.coins.push(coin);
        }
        draft.validate()?;
        if self.selected_coin.is_none() {
            self.selected_coin = Some(draft.coins.len() - 1);
        }
        self.draft = draft;
        Ok(())
    }
    fn commit_source(&mut self, cx: &App) -> AResult<()> {
        let (name, source) = self.source.read(cx)?;
        if self.selected_source.is_none() && name.is_empty() && source.url.is_empty() {
            return Ok(());
        }
        ensure!(!name.is_empty(), "请填写数据源名称。");
        let mut draft = self.draft.clone();
        if self.selected_source.as_deref() != Some(&name) {
            ensure!(!draft.sources.contains_key(&name), "数据源名称已存在。");
            if let Some(old) = &self.selected_source {
                draft.sources.remove(old);
                for coin in &mut draft.coins {
                    if coin.source == *old {
                        coin.source = name.clone();
                    }
                }
            }
        }
        draft.sources.insert(name.clone(), source);
        draft.validate()?;
        self.draft = draft;
        self.selected_source = Some(name);
        Ok(())
    }
    fn commit_page(&mut self, cx: &App) -> AResult<()> {
        match self.page {
            0 => self.commit_coin(cx),
            1 => self.commit_source(cx),
            _ => Ok(()),
        }
    }
    fn report(&mut self, result: AResult<()>, cx: &mut Context<Self>) -> bool {
        self.error = result.err().map(|e| format!("{e:#}"));
        cx.notify();
        self.error.is_none()
    }
    fn set_page(&mut self, page: usize, window: &mut Window, cx: &mut Context<Self>) {
        let result = self.commit_page(cx);
        if !self.report(result, cx) {
            return;
        }
        self.page = page;
        if page == 0 {
            let index = self
                .selected_coin
                .unwrap_or(0)
                .min(self.draft.coins.len() - 1);
            self.selected_coin = Some(index);
            self.coin = CoinForm::new(&self.draft.coins[index], &self.draft, window, cx);
        }
        cx.notify();
    }
    fn select_coin(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        let result = self.commit_coin(cx);
        if !self.report(result, cx) {
            return;
        }
        self.selected_coin = Some(index);
        self.coin = CoinForm::new(&self.draft.coins[index], &self.draft, window, cx);
        cx.notify();
    }
    fn add_coin(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let result = self.commit_coin(cx);
        if !self.report(result, cx) {
            return;
        }
        self.selected_coin = None;
        let coin = Coin {
            label: String::new(),
            symbol: String::new(),
            source: self.draft.sources.keys().next().unwrap().clone(),
        };
        self.coin = CoinForm::new(&coin, &self.draft, window, cx);
        cx.notify();
    }
    fn remove_coin(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.draft.coins.len() <= 1 && self.selected_coin.is_some() {
            self.error = Some("至少保留一个币种。".into());
            cx.notify();
            return;
        }
        if let Some(index) = self.selected_coin {
            self.draft.coins.remove(index);
        }
        let index = self
            .selected_coin
            .unwrap_or(0)
            .min(self.draft.coins.len() - 1);
        self.selected_coin = Some(index);
        self.coin = CoinForm::new(&self.draft.coins[index], &self.draft, window, cx);
        self.error = None;
        cx.notify();
    }
    fn move_coin(&mut self, direction: isize, window: &mut Window, cx: &mut Context<Self>) {
        let result = self.commit_coin(cx);
        if !self.report(result, cx) {
            return;
        }
        if let Some(index) = self.selected_coin {
            let next = index as isize + direction;
            if next >= 0 && (next as usize) < self.draft.coins.len() {
                self.draft.coins.swap(index, next as usize);
                self.selected_coin = Some(next as usize);
                self.coin =
                    CoinForm::new(&self.draft.coins[next as usize], &self.draft, window, cx);
            }
        }
        cx.notify();
    }
    fn select_source(&mut self, name: String, window: &mut Window, cx: &mut Context<Self>) {
        let result = self.commit_source(cx);
        if !self.report(result, cx) {
            return;
        }
        if let Some(source) = self.draft.sources.get(&name) {
            self.source = SourceForm::new(&name, source, window, cx);
            self.selected_source = Some(name);
        }
        cx.notify();
    }
    fn add_source(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let result = self.commit_source(cx);
        if !self.report(result, cx) {
            return;
        }
        self.selected_source = None;
        self.source = SourceForm::new("", &Source::default(), window, cx);
        cx.notify();
    }
    fn remove_source(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(name) = &self.selected_source {
            if self.draft.coins.iter().any(|c| c.source == *name) {
                self.error = Some("还有币种使用这个数据源，请先更换。".into());
                cx.notify();
                return;
            }
            self.draft.sources.remove(name);
        }
        let name = self.draft.sources.keys().next().unwrap().clone();
        self.source = SourceForm::new(&name, &self.draft.sources[&name], window, cx);
        self.selected_source = Some(name);
        self.error = None;
        cx.notify();
    }
    fn save(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let result = (|| -> AResult<()> {
            self.commit_page(cx)?;
            let mut config = self.draft.clone();
            self.display.read(&mut config, cx)?;
            config.save(&self.path)?;
            crate::platform::reload(&self.notify_class);
            Ok(())
        })();
        if self.report(result, cx) {
            window.remove_window();
            crate::platform::quit(cx);
        }
    }
    fn coins_page(&self, cx: &mut Context<Self>) -> AnyElement {
        let list = self.draft.coins.iter().enumerate().map(|(index, coin)| {
            ListItem::new(("coin", index))
                .selected(self.selected_coin == Some(index))
                .accessibility_label(format!("{} {}", coin.label, coin.symbol))
                .py_2()
                .child(
                    v_flex()
                        .gap_1()
                        .child(
                            div()
                                .font_weight(FontWeight::SEMIBOLD)
                                .child(coin.label.clone()),
                        )
                        .child(
                            div()
                                .text_sm()
                                .text_color(cx.theme().muted_foreground)
                                .child(coin.symbol.clone()),
                        ),
                )
                .on_click(
                    cx.listener(move |this, _, window, cx| this.select_coin(index, window, cx)),
                )
        });
        h_flex()
            .size_full()
            .items_start()
            .gap_6()
            .child(
                v_flex()
                    .w(px(228.))
                    .h_full()
                    .gap_3()
                    .child(
                        v_flex()
                            .id("coin-list")
                            .flex_1()
                            .min_h_0()
                            .overflow_y_scroll()
                            .children(list),
                    )
                    .child(
                        h_flex()
                            .gap_2()
                            .child(Button::new("coin-add").label("添加").on_click(
                                cx.listener(|this, _, window, cx| this.add_coin(window, cx)),
                            ))
                            .child(Button::new("coin-remove").label("删除").on_click(
                                cx.listener(|this, _, window, cx| this.remove_coin(window, cx)),
                            )),
                    )
                    .child(
                        h_flex()
                            .gap_2()
                            .child(Button::new("coin-up").label("上移").on_click(
                                cx.listener(|this, _, window, cx| this.move_coin(-1, window, cx)),
                            ))
                            .child(Button::new("coin-down").label("下移").on_click(
                                cx.listener(|this, _, window, cx| this.move_coin(1, window, cx)),
                            )),
                    ),
            )
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .gap_5()
                    .child(field("显示名称", Input::new(&self.coin.label)))
                    .child(field("交易对", Input::new(&self.coin.symbol)))
                    .child(field("数据源", Select::new(&self.coin.source).w_full())),
            )
            .into_any_element()
    }
    fn sources_page(&self, cx: &mut Context<Self>) -> AnyElement {
        let list = self.draft.sources.keys().map(|name| {
            let name = name.clone();
            ListItem::new(SharedString::from(format!("source-{name}")))
                .selected(self.selected_source.as_ref() == Some(&name))
                .accessibility_label(name.clone())
                .child(name.clone())
                .on_click(cx.listener(move |this, _, window, cx| {
                    this.select_source(name.clone(), window, cx)
                }))
        });
        h_flex()
            .size_full()
            .items_start()
            .gap_6()
            .child(
                v_flex()
                    .w(px(180.))
                    .h_full()
                    .gap_3()
                    .child(
                        v_flex()
                            .id("source-list")
                            .flex_1()
                            .min_h_0()
                            .overflow_y_scroll()
                            .children(list),
                    )
                    .child(
                        h_flex()
                            .gap_2()
                            .child(Button::new("source-add").label("添加").on_click(
                                cx.listener(|this, _, window, cx| this.add_source(window, cx)),
                            ))
                            .child(Button::new("source-remove").label("删除").on_click(
                                cx.listener(|this, _, window, cx| this.remove_source(window, cx)),
                            )),
                    ),
            )
            .child(
                v_flex()
                    .id("source-form")
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .overflow_y_scroll()
                    .gap_4()
                    .pr_2()
                    .child(row(
                        field("名称", Input::new(&self.source.name)),
                        field("接口代码格式", Select::new(&self.source.style).w_full()),
                    ))
                    .child(field("接口 URL", Input::new(&self.source.url)))
                    .child(row(
                        field("价格路径", Input::new(&self.source.price)),
                        field("涨跌幅路径", Input::new(&self.source.change)),
                    ))
                    .child(row(
                        field("开盘价路径（24h，可选）", Input::new(&self.source.open)),
                        field(
                            "历史价格路径（可选）",
                            Input::new(&self.source.history_price),
                        ),
                    ))
                    .child(field(
                        "历史接口 URL（其他周期，可选）",
                        Input::new(&self.source.history),
                    ))
                    .child(field(
                        "请求头（每行 名称: 值）",
                        Textarea::new(&self.source.headers),
                    ))
                    .child(field(
                        "代码映射（BTCUSDT=bitcoin）",
                        Textarea::new(&self.source.mapping),
                    )),
            )
            .into_any_element()
    }
    fn display_page(&self, cx: &mut Context<Self>) -> AnyElement {
        v_flex()
            .id("display-form")
            .size_full()
            .overflow_y_scroll()
            .gap_5()
            .pr_2()
            .child(row(
                field("行情刷新（秒）", Input::new(&self.display.refresh)),
                field("轮播间隔（秒）", Input::new(&self.display.rotation)),
            ))
            .child(row(
                field("涨跌幅周期", Select::new(&self.display.period).w_full()),
                field("价格前缀", Input::new(&self.display.currency)),
            ))
            .child(row(
                field("显示宽度", Input::new(&self.display.width)),
                field("边缘距离", Input::new(&self.display.offset)),
            ))
            .child(row(
                field("显示位置", Select::new(&self.display.position).w_full()),
                field("主题", Select::new(&self.display.theme).w_full()),
            ))
            .child(row(
                field("上涨颜色", ColorSelect::new(&self.display.up).w_full()),
                field("下跌颜色", ColorSelect::new(&self.display.down).w_full()),
            ))
            .child(
                Checkbox::new("show-change")
                    .label("任务栏显示涨跌幅")
                    .checked(self.display.show_change)
                    .on_click(cx.listener(|this, checked: &bool, _, cx| {
                        this.display.show_change = *checked;
                        cx.notify();
                    })),
            )
            .into_any_element()
    }
}
impl Render for Settings {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let content = match self.page {
            0 => self.coins_page(cx),
            1 => self.sources_page(cx),
            _ => self.display_page(cx),
        };
        v_flex()
            .size_full()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .font_family("Segoe UI")
            .text_base()
            .child(
                h_flex()
                    .px_6()
                    .py_4()
                    .gap_2()
                    .border_b_1()
                    .border_color(cx.theme().border)
                    .children(["自选币种", "数据源", "显示"].into_iter().enumerate().map(
                        |(index, label)| {
                            Button::new(("tab", index))
                                .label(label)
                                .ghost()
                                .selected(self.page == index)
                                .on_click(cx.listener(move |this, _, window, cx| {
                                    this.set_page(index, window, cx)
                                }))
                        },
                    )),
            )
            .child(div().flex_1().min_h_0().p_6().child(content))
            .child(
                h_flex()
                    .px_6()
                    .py_4()
                    .gap_3()
                    .border_t_1()
                    .border_color(cx.theme().border)
                    .child(
                        div()
                            .flex_1()
                            .text_sm()
                            .text_color(cx.theme().danger)
                            .child(self.error.clone().unwrap_or_default()),
                    )
                    .child(
                        Button::new("cancel")
                            .label("取消")
                            .on_click(|_, window, cx| {
                                window.remove_window();
                                crate::platform::quit(cx);
                            }),
                    )
                    .child(
                        Button::new("save")
                            .label("保存")
                            .primary()
                            .on_click(cx.listener(|this, _, window, cx| this.save(window, cx))),
                    ),
            )
    }
}
