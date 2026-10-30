//! All window state stays on the Windows message-loop thread. Reentrant
//! messages fall back to DefWindowProc rather than borrowing state twice.
#![allow(unsafe_op_in_unsafe_fn)]
use crate::paint::{Gdi, Painter};
use anyhow::{Result, ensure};
use crypto_taskbar::engine::{Engine, Snapshot};
use crypto_taskbar_core::{Config, format_change, format_price, parse_color};
use std::{
    cell::RefCell,
    ffi::c_void,
    path::PathBuf,
    process::{Child, Command},
    ptr::{null, null_mut},
    time::{Duration, Instant, SystemTime},
};
use windows_sys::Win32::{
    Foundation::*,
    Graphics::Gdi::*,
    System::{LibraryLoader::GetModuleHandleW, Registry::*, Threading::*},
    UI::{HiDpi::*, Shell::ShellExecuteW, WindowsAndMessaging::*},
};

const UPDATED: u32 = WM_APP + 2;
const OPEN_SETTINGS: u32 = WM_APP + 4;
const SHOW_LIST: u32 = WM_APP + 1;
const REFRESH: u32 = 101;
const SETTINGS: u32 = 102;
const STARTUP: u32 = 103;
const EXIT: u32 = 104;
const FOLDER: u32 = 105;
thread_local! { static UI:RefCell<Option<App>>=const {RefCell::new(None)}; }
fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}
fn wide_path(path: &std::path::Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str().encode_wide().chain(Some(0)).collect()
}
pub fn message(text: &str) {
    unsafe {
        MessageBoxW(
            null_mut(),
            wide(text).as_ptr(),
            wide("Crypto Taskbar").as_ptr(),
            MB_OK | MB_ICONERROR,
        );
    }
}
pub struct Options {
    pub path: PathBuf,
    pub settings: bool,
    pub preview: Option<bool>,
    pub test: bool,
    pub inspect: bool,
}
struct App {
    overlay: HWND,
    popup: HWND,
    class: Vec<u16>,
    path: PathBuf,
    initial: Config,
    engine: Option<Engine>,
    settings: Option<Child>,
    modified: Option<SystemTime>,
    scale: f32,
    dark: bool,
    bar: RECT,
    work: RECT,
    coin: usize,
    scroll: usize,
    wheel: i32,
    rotated: Instant,
    animating: bool,
    hover: Option<Instant>,
    leave: Option<Instant>,
    pinned: bool,
    menu_open: bool,
    preview: Option<bool>,
    last_check: Instant,
    config_error: Option<String>,
    reload: u32,
    taskbar_created: u32,
    inspect: bool,
}
impl App {
    fn snapshot(&self) -> Snapshot {
        self.engine
            .as_ref()
            .map(Engine::snapshot)
            .unwrap_or_else(|| Snapshot {
                config: self.initial.clone(),
                quotes: vec![Default::default(); self.initial.coins.len()],
            })
    }
    fn px(&self, n: f32) -> i32 {
        (n * self.scale).round() as i32
    }
    fn fg(&self) -> u32 {
        if self.dark { 0xfff0f2f6 } else { 0xff181d25 }
    }
    fn muted(&self) -> u32 {
        if self.dark { 0xffa2a9b5 } else { 0xff5e6776 }
    }
    fn tone(&self, change: f64, c: &Config) -> u32 {
        0xff000000
            | parse_color(if change >= 0. {
                &c.up_color
            } else {
                &c.down_color
            })
            .unwrap_or(0x808080)
    }
    fn rows(&self) -> usize {
        ((self.work.bottom - self.work.top - self.px(108.)) / self.px(54.).max(1)).clamp(1, 12)
            as usize
    }
    unsafe fn render(&mut self, hwnd: HWND) {
        let mut rect: RECT = std::mem::zeroed();
        GetClientRect(hwnd, &mut rect);
        let (w, h) = (rect.right as f32, rect.bottom as f32);
        let Some(p) = Painter::new(
            rect.right,
            rect.bottom,
            self.scale,
            if hwnd == self.overlay { 0x01000000 } else { 0 },
        ) else {
            return;
        };
        let snapshot = self.snapshot();
        let c = &snapshot.config;
        let quotes = &snapshot.quotes;
        if hwnd == self.overlay {
            self.coin %= c.coins.len();
            let draw = |i: usize, y: f32| {
                let q = &quotes[i];
                let label = format!("{}{}", c.coins[i].label, if q.stale { " ·" } else { "" });
                let price = q
                    .price
                    .map(|v| format!("{}{}", c.currency, format_price(v)))
                    .unwrap_or_else(|| "—".into());
                let area = w - p.px(18.);
                p.text(
                    &label,
                    [p.px(9.), y, area, h * 0.43],
                    10.,
                    self.muted(),
                    false,
                    false,
                );
                let change = if c.show_change {
                    q.change.map(format_change)
                } else {
                    None
                };
                let change_width = change
                    .as_ref()
                    .map(|s| p.measure(s, 11., false).min(area))
                    .unwrap_or(0.);
                let price_width = if change.is_some() {
                    p.measure(&price, 13., true)
                        .min((area - change_width - p.px(6.)).max(0.))
                } else {
                    area
                };
                p.text(
                    &price,
                    [p.px(9.), y + h * 0.34, price_width, h * 0.6],
                    13.,
                    if q.stale { self.muted() } else { self.fg() },
                    true,
                    false,
                );
                if let Some(change) = change {
                    p.text(
                        &change,
                        [p.px(15.) + price_width, y + h * 0.34, change_width, h * 0.6],
                        11.,
                        if q.stale {
                            self.muted()
                        } else {
                            self.tone(q.change.unwrap_or(0.), c)
                        },
                        false,
                        false,
                    );
                }
            };
            if self.animating {
                let t = (self.rotated.elapsed().as_secs_f32() / 0.26).clamp(0., 1.);
                let t = t * t * (3. - 2. * t);
                draw(self.coin, -t * h);
                draw((self.coin + 1) % c.coins.len(), (1. - t) * h);
            } else {
                draw(self.coin, 0.);
            }
        } else {
            p.rounded(
                [1., 1., w - 2., h - 2.],
                p.px(14.),
                if self.dark { 0xfc1f2228 } else { 0xfdf9fafc },
            );
            p.text(
                "自选行情",
                [p.px(20.), p.px(10.), p.px(160.), p.px(28.)],
                13.,
                self.fg(),
                true,
                false,
            );
            p.text(
                &c.change_period,
                [w - p.px(100.), p.px(10.), p.px(80.), p.px(28.)],
                10.,
                self.muted(),
                false,
                true,
            );
            let rows = self.rows().min(c.coins.len());
            self.scroll = self.scroll.min(c.coins.len() - rows);
            for row in 0..rows {
                let i = row + self.scroll;
                let q = &quotes[i];
                let y = p.px(46. + row as f32 * 54.);
                p.text(
                    &c.coins[i].label,
                    [p.px(20.), y, p.px(95.), p.px(24.)],
                    13.,
                    self.fg(),
                    true,
                    false,
                );
                let mut status = c.coins[i].source.clone();
                if let Some(error) = q.error.as_ref().or(q.change_error.as_ref()) {
                    status.push_str(" · ");
                    status.push_str(error);
                } else if q.price.is_none() {
                    status.push_str(" · 正在读取");
                }
                p.text(
                    &status,
                    [p.px(20.), y + p.px(22.), w - p.px(130.), p.px(18.)],
                    10.,
                    self.muted(),
                    false,
                    false,
                );
                let price = q
                    .price
                    .map(|v| format!("{}{}", c.currency, format_price(v)))
                    .unwrap_or_else(|| "—".into());
                p.text(
                    &price,
                    [p.px(120.), y, w - p.px(140.), p.px(24.)],
                    13.,
                    if q.stale { self.muted() } else { self.fg() },
                    true,
                    true,
                );
                p.text(
                    &q.change.map(format_change).unwrap_or_else(|| "—".into()),
                    [w - p.px(110.), y + p.px(23.), p.px(90.), p.px(18.)],
                    11.,
                    match q.change {
                        Some(change) if !q.stale => self.tone(change, c),
                        _ => self.muted(),
                    },
                    false,
                    true,
                );
            }
            let failed = quotes.iter().filter(|q| q.error.is_some()).count();
            let oldest = quotes.iter().filter_map(|q| q.updated).min();
            let mut footer = oldest
                .map(|t| format!("更新于 {}", local_time(t)))
                .unwrap_or_else(|| "正在读取行情".into());
            if failed > 0 {
                footer = if oldest.is_some() {
                    format!(
                        "{} · {}",
                        footer,
                        if failed == quotes.len() {
                            "数据未更新"
                        } else {
                            "部分数据未更新"
                        }
                    )
                } else {
                    "读取失败，请检查数据源或网络".into()
                };
            }
            if self.config_error.is_some() {
                footer = "配置读取失败，仍使用上次配置".into();
            }
            p.text(
                &footer,
                [p.px(20.), h - p.px(32.), w - p.px(100.), p.px(22.)],
                10.,
                self.muted(),
                false,
                false,
            );
            if c.coins.len() > rows {
                p.text(
                    &format!(
                        "{}–{} / {}",
                        self.scroll + 1,
                        self.scroll + rows,
                        c.coins.len()
                    ),
                    [w - p.px(100.), h - p.px(32.), p.px(80.), p.px(22.)],
                    10.,
                    self.muted(),
                    false,
                    true,
                );
            }
        }
        p.present(hwnd);
    }
    unsafe fn show_popup(&mut self, pinned: bool) {
        self.pinned = pinned;
        let c = self.snapshot().config;
        let rows = self.rows().min(c.coins.len());
        let width = self.px(360.);
        let height = self.px(82. + rows as f32 * 54.);
        let mut anchor: RECT = std::mem::zeroed();
        GetWindowRect(self.overlay, &mut anchor);
        let x = anchor.left.clamp(
            self.work.left,
            (self.work.right - width).max(self.work.left),
        );
        let y = (self.bar.top - height - self.px(8.)).clamp(
            self.work.top,
            (self.work.bottom - height).max(self.work.top),
        );
        SetWindowPos(
            self.popup,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW,
        );
        self.render(self.popup);
    }
    unsafe fn fullscreen(&self) -> bool {
        let fg = GetForegroundWindow();
        if fg.is_null() || fg == self.overlay || fg == self.popup || fg == GetShellWindow() {
            return false;
        }
        if self.settings.as_ref().is_some_and(|c| {
            let mut pid = 0;
            GetWindowThreadProcessId(fg, &mut pid);
            pid == c.id()
        }) {
            return false;
        }
        let mut class = [0u16; 64];
        let n = GetClassNameW(fg, class.as_mut_ptr(), 64);
        let class = String::from_utf16_lossy(&class[..n.max(0) as usize]);
        if ["Shell_TrayWnd", "WorkerW", "Progman"].contains(&class.as_str()) {
            return false;
        }
        let monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
        if monitor != MonitorFromRect(&self.bar, MONITOR_DEFAULTTOPRIMARY) {
            return false;
        }
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        GetMonitorInfoW(monitor, &mut info);
        let mut r: RECT = std::mem::zeroed();
        GetWindowRect(fg, &mut r);
        r.left <= info.rcMonitor.left
            && r.top <= info.rcMonitor.top
            && r.right >= info.rcMonitor.right
            && r.bottom >= info.rcMonitor.bottom
    }
    unsafe fn place(&mut self) {
        if self.menu_open {
            return;
        }
        let taskbar = FindWindowW(wide("Shell_TrayWnd").as_ptr(), null());
        if taskbar.is_null() || GetWindowRect(taskbar, &mut self.bar) == 0 {
            ShowWindow(self.overlay, SW_HIDE);
            ShowWindow(self.popup, SW_HIDE);
            return;
        }
        let scale = GetDpiForWindow(taskbar) as f32 / 96.;
        let scale_changed = self.scale != scale;
        self.scale = scale.max(1.);
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        GetMonitorInfoW(
            MonitorFromRect(&self.bar, MONITOR_DEFAULTTOPRIMARY),
            &mut info,
        );
        self.work = info.rcWork;
        if self.preview.is_none() && self.fullscreen()
            || self.bar.bottom - self.bar.top > self.px(100.)
            || self.bar.top < info.rcMonitor.top + (info.rcMonitor.bottom - info.rcMonitor.top) / 2
            || self.bar.top >= info.rcMonitor.bottom - 2
        {
            ShowWindow(self.overlay, SW_HIDE);
            ShowWindow(self.popup, SW_HIDE);
            return;
        }
        let c = self.snapshot().config;
        let width = self.px(c.width as f32).min(self.bar.right - self.bar.left);
        let height = self.px(46.).min(self.bar.bottom - self.bar.top);
        let mut x = self.bar.left + self.px(c.offset as f32);
        if c.position == "right" {
            let tray = FindWindowExW(taskbar, null_mut(), wide("TrayNotifyWnd").as_ptr(), null());
            let mut r: RECT = std::mem::zeroed();
            let right = if !tray.is_null() && GetWindowRect(tray, &mut r) != 0 {
                r.left
            } else {
                self.bar.right - self.px(220.)
            };
            x = right - self.px((c.offset + c.width) as f32);
        }
        x = x.clamp(self.bar.left, (self.bar.right - width).max(self.bar.left));
        SetWindowPos(
            self.overlay,
            HWND_TOPMOST,
            x,
            self.bar.top + (self.bar.bottom - self.bar.top - height) / 2,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW,
        );
        let dark = self.preview.unwrap_or_else(|| match c.theme.as_str() {
            "light" => false,
            "dark" => true,
            _ => system_dark(),
        });
        if dark != self.dark || scale_changed {
            self.dark = dark;
            self.render(self.overlay);
            if IsWindowVisible(self.popup) != 0 {
                self.show_popup(self.pinned);
            }
        }
    }
    unsafe fn reload(&mut self) {
        self.modified = std::fs::metadata(&self.path)
            .and_then(|m| m.modified())
            .ok();
        match Config::load(&self.path) {
            Ok(config) => {
                self.initial = config.clone();
                if let Some(engine) = &self.engine {
                    engine.apply(config);
                }
                self.coin = 0;
                self.scroll = 0;
                self.animating = false;
                self.rotated = Instant::now();
                KillTimer(self.overlay, 2);
                self.config_error = None;
                self.place();
                self.render(self.overlay);
            }
            Err(e) => self.config_error = Some(e.to_string()),
        }
    }
    unsafe fn tick(&mut self) {
        if self.menu_open {
            return;
        }
        self.place();
        if self.last_check.elapsed() > Duration::from_secs(1) {
            self.last_check = Instant::now();
            let modified = std::fs::metadata(&self.path)
                .and_then(|m| m.modified())
                .ok();
            if modified != self.modified {
                self.reload();
            }
        }
        if let Some(child) = &mut self.settings
            && child.try_wait().ok().flatten().is_some()
        {
            self.settings = None;
        }
        let mut point: POINT = std::mem::zeroed();
        GetCursorPos(&mut point);
        let over = contains(self.overlay, point);
        let in_list = contains(self.popup, point);
        if self.settings.is_some() {
            return;
        }
        if over {
            self.leave = None;
            let start = self.hover.get_or_insert_with(Instant::now);
            if start.elapsed() >= Duration::from_millis(300) && IsWindowVisible(self.popup) == 0 {
                self.show_popup(false);
            }
        } else {
            self.hover = None;
            if in_list {
                self.leave = None;
            } else if IsWindowVisible(self.popup) != 0 && !self.pinned {
                let start = self.leave.get_or_insert_with(Instant::now);
                if start.elapsed() >= Duration::from_millis(350) {
                    ShowWindow(self.popup, SW_HIDE);
                }
            }
        }
        let c = self.snapshot().config;
        if !self.animating
            && !over
            && IsWindowVisible(self.popup) == 0
            && c.coins.len() > 1
            && self.rotated.elapsed() >= Duration::from_secs(c.rotation_seconds)
        {
            self.rotated = Instant::now();
            self.animating = true;
            SetTimer(self.overlay, 2, 16, None);
        }
    }
    unsafe fn open_settings(&mut self) {
        if let Some(child) = &mut self.settings
            && child.try_wait().ok().flatten().is_none()
        {
            let hwnd = process_window(child.id());
            if !hwnd.is_null() {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            }
            return;
        }
        let result = (|| -> Result<Child> {
            let executable = std::env::current_exe()?.with_file_name("CryptoTaskbarSettings.exe");
            ensure!(
                executable.is_file(),
                "找不到配置程序，请将两个 EXE 放在同一文件夹。"
            );
            use std::os::windows::process::CommandExt;
            let mut command = Command::new(executable);
            if self.inspect {
                command.arg("--inspect");
            }
            Ok(command
                .arg("--config")
                .arg(&self.path)
                .arg("--notify-class")
                .arg(String::from_utf16_lossy(
                    &self.class[..self.class.len() - 1],
                ))
                .creation_flags(CREATE_NO_WINDOW)
                .spawn()?)
        })();
        match result {
            Ok(child) => self.settings = Some(child),
            Err(error) => message(&error.to_string()),
        }
    }
    unsafe fn menu(&mut self) {
        if self.menu_open {
            return;
        }
        self.menu_open = true;
        self.pinned = false;
        ShowWindow(self.popup, SW_HIDE);
        let menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, SETTINGS as usize, wide("配置…").as_ptr());
        AppendMenuW(menu, MF_STRING, REFRESH as usize, wide("立即刷新").as_ptr());
        AppendMenuW(
            menu,
            MF_STRING,
            FOLDER as usize,
            wide("打开配置文件夹").as_ptr(),
        );
        AppendMenuW(
            menu,
            MF_STRING | if startup_enabled() { MF_CHECKED } else { 0 },
            STARTUP as usize,
            wide("开机启动").as_ptr(),
        );
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        AppendMenuW(menu, MF_STRING, EXIT as usize, wide("退出").as_ptr());
        let mut point: POINT = std::mem::zeroed();
        GetCursorPos(&mut point);
        SetForegroundWindow(self.overlay);
        let id = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x,
            point.y,
            0,
            self.overlay,
            null(),
        ) as u32;
        DestroyMenu(menu);
        PostMessageW(self.overlay, WM_NULL, 0, 0);
        self.menu_open = false;
        match id {
            SETTINGS => self.open_settings(),
            REFRESH => {
                if let Some(engine) = &self.engine {
                    engine.refresh();
                }
            }
            STARTUP => {
                if let Err(e) = toggle_startup() {
                    message(&e.to_string());
                }
            }
            FOLDER => {
                if let Some(parent) = self.path.parent() {
                    ShellExecuteW(
                        null_mut(),
                        wide("open").as_ptr(),
                        wide_path(parent).as_ptr(),
                        null(),
                        null(),
                        SW_SHOWNORMAL,
                    );
                }
            }
            EXIT => {
                PostMessageW(self.overlay, WM_CLOSE, 0, 0);
            }
            _ => {}
        }
    }
}
unsafe fn contains(hwnd: HWND, point: POINT) -> bool {
    let mut r: RECT = std::mem::zeroed();
    IsWindowVisible(hwnd) != 0
        && GetWindowRect(hwnd, &mut r) != 0
        && point.x >= r.left
        && point.x < r.right
        && point.y >= r.top
        && point.y < r.bottom
}
fn system_dark() -> bool {
    unsafe {
        let mut light = 0u32;
        let mut size = 4;
        RegGetValueW(
            HKEY_CURRENT_USER,
            wide("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize").as_ptr(),
            wide("SystemUsesLightTheme").as_ptr(),
            RRF_RT_REG_DWORD,
            null_mut(),
            &mut light as *mut _ as *mut c_void,
            &mut size,
        );
        light == 0
    }
}
fn local_time(seconds: u64) -> String {
    unsafe {
        let value = (seconds + 11644473600) * 10000000;
        let ft = FILETIME {
            dwLowDateTime: value as u32,
            dwHighDateTime: (value >> 32) as u32,
        };
        let mut utc: SYSTEMTIME = std::mem::zeroed();
        let mut local: SYSTEMTIME = std::mem::zeroed();
        windows_sys::Win32::System::Time::FileTimeToSystemTime(&ft, &mut utc);
        windows_sys::Win32::System::Time::SystemTimeToTzSpecificLocalTime(null(), &utc, &mut local);
        format!(
            "{:02}:{:02}:{:02}",
            local.wHour, local.wMinute, local.wSecond
        )
    }
}
unsafe fn process_window(pid: u32) -> HWND {
    struct Search {
        pid: u32,
        hwnd: HWND,
    }
    unsafe extern "system" fn callback(hwnd: HWND, param: LPARAM) -> i32 {
        let search = &mut *(param as *mut Search);
        let mut pid = 0;
        GetWindowThreadProcessId(hwnd, &mut pid);
        if pid == search.pid && IsWindowVisible(hwnd) != 0 {
            search.hwnd = hwnd;
            return 0;
        }
        1
    }
    let mut search = Search {
        pid,
        hwnd: null_mut(),
    };
    EnumWindows(Some(callback), &mut search as *mut _ as LPARAM);
    search.hwnd
}
unsafe fn startup_enabled() -> bool {
    let mut size = 0;
    RegGetValueW(
        HKEY_CURRENT_USER,
        wide("Software\\Microsoft\\Windows\\CurrentVersion\\Run").as_ptr(),
        wide("CryptoTaskbar").as_ptr(),
        RRF_RT_REG_SZ,
        null_mut(),
        null_mut(),
        &mut size,
    ) == 0
}
unsafe fn toggle_startup() -> Result<()> {
    let mut key = null_mut();
    ensure!(
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            wide("Software\\Microsoft\\Windows\\CurrentVersion\\Run").as_ptr(),
            0,
            null(),
            0,
            KEY_SET_VALUE,
            null(),
            &mut key,
            null_mut()
        ) == 0,
        "无法修改开机启动"
    );
    let status = if startup_enabled() {
        RegDeleteValueW(key, wide("CryptoTaskbar").as_ptr())
    } else {
        let command = wide(&format!("\"{}\"", std::env::current_exe()?.display()));
        RegSetValueExW(
            key,
            wide("CryptoTaskbar").as_ptr(),
            0,
            REG_SZ,
            command.as_ptr() as *const u8,
            (command.len() * 2) as u32,
        )
    };
    RegCloseKey(key);
    ensure!(status == 0, "无法保存开机启动设置");
    Ok(())
}

unsafe extern "system" fn window_proc(hwnd: HWND, msg: u32, w: WPARAM, l: LPARAM) -> LRESULT {
    UI.with(|cell| {
        let Ok(mut guard) = cell.try_borrow_mut() else {
            return DefWindowProcW(hwnd, msg, w, l);
        };
        let Some(app) = guard.as_mut() else {
            return DefWindowProcW(hwnd, msg, w, l);
        };
        if msg == app.reload {
            app.reload();
            return 0;
        }
        if msg == app.taskbar_created {
            app.place();
            app.render(app.overlay);
            return 0;
        }
        match msg {
            WM_MOUSEACTIVATE => {
                if hwnd == app.popup {
                    MA_ACTIVATE as isize
                } else {
                    MA_NOACTIVATE as isize
                }
            }
            WM_NCHITTEST if hwnd == app.overlay => HTCLIENT as isize,
            WM_ERASEBKGND => 1,
            WM_PAINT => {
                let mut paint = std::mem::zeroed();
                BeginPaint(hwnd, &mut paint);
                EndPaint(hwnd, &paint);
                0
            }
            WM_TIMER => {
                if hwnd == app.overlay {
                    match w {
                        1 => app.tick(),
                        2 => {
                            if app.rotated.elapsed() >= Duration::from_millis(260) {
                                app.coin = (app.coin + 1) % app.snapshot().config.coins.len();
                                app.rotated = Instant::now();
                                app.animating = false;
                                KillTimer(hwnd, 2);
                            }
                            app.render(hwnd);
                        }
                        99 => {
                            app.show_popup(true);
                            KillTimer(hwnd, 99);
                        }
                        _ => {}
                    }
                }
                0
            }
            UPDATED => {
                app.place();
                app.render(app.overlay);
                if IsWindowVisible(app.popup) != 0 {
                    app.show_popup(app.pinned);
                }
                0
            }
            WM_SETTINGCHANGE | WM_THEMECHANGED | WM_DISPLAYCHANGE | WM_DPICHANGED => {
                app.place();
                app.render(app.overlay);
                if IsWindowVisible(app.popup) != 0 {
                    app.show_popup(app.pinned);
                }
                0
            }
            WM_LBUTTONUP => {
                if hwnd == app.overlay {
                    if app.pinned && IsWindowVisible(app.popup) != 0 {
                        app.pinned = false;
                        ShowWindow(app.popup, SW_HIDE);
                    } else {
                        app.show_popup(true);
                    }
                }
                0
            }
            WM_RBUTTONUP | WM_CONTEXTMENU => {
                app.menu();
                0
            }
            WM_MOUSEWHEEL => {
                let delta = ((w >> 16) as u16) as i16 as i32;
                app.wheel += delta;
                let steps = app.wheel / 120;
                app.wheel %= 120;
                if hwnd == app.popup {
                    app.scroll = (app.scroll as i32 - steps * 3).max(0) as usize;
                    app.render(hwnd);
                } else if steps != 0 {
                    let count = app.snapshot().config.coins.len() as i32;
                    app.coin = (app.coin as i32 - steps).rem_euclid(count) as usize;
                    app.animating = false;
                    app.rotated = Instant::now();
                    KillTimer(hwnd, 2);
                    app.render(hwnd);
                }
                0
            }
            WM_KEYDOWN if hwnd == app.popup => {
                match w as u32 {
                    0x1b => {
                        app.pinned = false;
                        ShowWindow(hwnd, SW_HIDE);
                    }
                    0x26 => app.scroll = app.scroll.saturating_sub(1),
                    0x28 => app.scroll += 1,
                    0x21 => app.scroll = app.scroll.saturating_sub(app.rows()),
                    0x22 => app.scroll += app.rows(),
                    0x24 => app.scroll = 0,
                    0x23 => app.scroll = 100,
                    _ => {}
                }
                app.render(hwnd);
                0
            }
            OPEN_SETTINGS => {
                app.open_settings();
                0
            }
            SHOW_LIST => {
                app.place();
                app.show_popup(true);
                0
            }
            WM_CLOSE => {
                ShowWindow(app.overlay, SW_HIDE);
                ShowWindow(app.popup, SW_HIDE);
                PostQuitMessage(0);
                0
            }
            _ => DefWindowProcW(hwnd, msg, w, l),
        }
    })
}

pub fn run(options: Options) -> Result<()> {
    unsafe {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        let mutex_name = if options.test || options.preview.is_some() {
            "Local\\CryptoTaskbar.RustTest"
        } else {
            "Local\\CryptoTaskbar.v1"
        };
        let class = wide(if options.test || options.preview.is_some() {
            "CryptoTaskbar.RustTest"
        } else {
            "CryptoTaskbar.Window.v1"
        });
        let mutex = CreateMutexW(null(), 0, wide(mutex_name).as_ptr());
        ensure!(!mutex.is_null(), "无法创建程序实例");
        if GetLastError() == ERROR_ALREADY_EXISTS {
            let existing = FindWindowW(class.as_ptr(), null());
            if !existing.is_null() {
                PostMessageW(
                    existing,
                    if options.settings {
                        OPEN_SETTINGS
                    } else {
                        SHOW_LIST
                    },
                    0,
                    0,
                );
            }
            CloseHandle(mutex);
            return Ok(());
        }
        let _gdi = Gdi::new()?;
        let config = if options.path.exists() {
            Config::load(&options.path)?
        } else {
            let config = Config::default();
            config.save(&options.path)?;
            config
        };
        let modified = std::fs::metadata(&options.path)
            .and_then(|m| m.modified())
            .ok();
        let instance = GetModuleHandleW(null());
        let mut window_class: WNDCLASSW = std::mem::zeroed();
        window_class.lpfnWndProc = Some(window_proc);
        window_class.hInstance = instance;
        window_class.lpszClassName = class.as_ptr();
        window_class.hCursor = LoadCursorW(null_mut(), IDC_ARROW);
        ensure!(RegisterClassW(&window_class) != 0, "无法注册窗口");
        let overlay = CreateWindowExW(
            WS_EX_LAYERED
                | if options.inspect {
                    WS_EX_APPWINDOW
                } else {
                    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
                },
            class.as_ptr(),
            wide("Crypto Taskbar").as_ptr(),
            WS_POPUP,
            0,
            0,
            1,
            1,
            null_mut(),
            null_mut(),
            instance,
            null(),
        );
        let popup = CreateWindowExW(
            WS_EX_LAYERED
                | if options.inspect {
                    WS_EX_APPWINDOW
                } else {
                    WS_EX_TOOLWINDOW
                },
            class.as_ptr(),
            wide("Crypto Taskbar — 自选行情").as_ptr(),
            WS_POPUP,
            0,
            0,
            1,
            1,
            overlay,
            null_mut(),
            instance,
            null(),
        );
        ensure!(!overlay.is_null() && !popup.is_null(), "无法创建行情窗口");
        let notify_handle = overlay as isize;
        let engine = if options.preview.is_some() {
            Engine::demo(config.clone())
        } else {
            Engine::new(config.clone(), move || {
                PostMessageW(notify_handle as HWND, UPDATED, 0, 0);
            })
        };
        UI.with(|cell| {
            *cell.borrow_mut() = Some(App {
                overlay,
                popup,
                class: class.clone(),
                path: options.path,
                initial: config,
                engine: Some(engine),
                settings: None,
                modified,
                scale: 1.,
                dark: true,
                bar: std::mem::zeroed(),
                work: std::mem::zeroed(),
                coin: 0,
                scroll: 0,
                wheel: 0,
                rotated: Instant::now(),
                animating: false,
                hover: None,
                leave: None,
                pinned: false,
                menu_open: false,
                preview: options.preview,
                last_check: Instant::now(),
                config_error: None,
                reload: RegisterWindowMessageW(wide("CryptoTaskbar.ReloadConfig.v2").as_ptr()),
                taskbar_created: RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()),
                inspect: options.inspect,
            });
        });
        UI.with(|cell| {
            let mut guard = cell.borrow_mut();
            let app = guard.as_mut().unwrap();
            app.place();
            app.render(overlay);
            if options.settings {
                app.open_settings();
            }
        });
        SetTimer(overlay, 1, 150, None);
        if options.preview.is_some() {
            SetTimer(overlay, 99, 350, None);
        }
        let mut msg: MSG = std::mem::zeroed();
        while GetMessageW(&mut msg, null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UI.with(|cell| {
            if let Some(mut app) = cell.borrow_mut().take() {
                drop(app.engine.take());
                if let Some(child) = &app.settings {
                    let hwnd = process_window(child.id());
                    if !hwnd.is_null() {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    }
                }
                DestroyWindow(popup);
                DestroyWindow(overlay);
            }
        });
        UnregisterClassW(class.as_ptr(), instance);
        CloseHandle(mutex);
        Ok(())
    }
}
