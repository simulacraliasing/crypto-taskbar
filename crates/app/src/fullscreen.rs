//! Detect fullscreen content on the taskbar's monitor, independently of focus.
use windows_sys::Win32::{
    Foundation::*,
    Graphics::{Dwm::*, Gdi::*},
    System::Threading::GetCurrentProcessId,
    UI::WindowsAndMessaging::*,
};

#[derive(Clone, Copy, Debug)]
pub struct Bounds {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}
impl From<RECT> for Bounds {
    fn from(r: RECT) -> Self {
        Self {
            left: r.left,
            top: r.top,
            right: r.right,
            bottom: r.bottom,
        }
    }
}
impl Bounds {
    fn intersects(self, other: Self) -> bool {
        self.left < self.right
            && self.top < self.bottom
            && self.left < other.right
            && self.right > other.left
            && self.top < other.bottom
            && self.bottom > other.top
    }
    fn covers(self, monitor: Self) -> bool {
        // DWM and mixed-DPI window transitions may round a visible edge by a pixel.
        const EPSILON: i64 = 2;
        i64::from(self.left) <= i64::from(monitor.left) + EPSILON
            && i64::from(self.top) <= i64::from(monitor.top) + EPSILON
            && i64::from(self.right) >= i64::from(monitor.right) - EPSILON
            && i64::from(self.bottom) >= i64::from(monitor.bottom) - EPSILON
    }
}

#[derive(Clone, Copy, Debug)]
pub struct WindowState {
    pub bounds: Bounds,
    pub visible: bool,
    pub minimized: bool,
    pub cloaked: bool,
    pub auxiliary: bool,
}

/// None means this window does not participate in the monitor's app Z order.
pub fn classify(monitor: Bounds, window: WindowState) -> Option<bool> {
    if !window.visible
        || window.minimized
        || window.cloaked
        || window.auxiliary
        || !window.bounds.intersects(monitor)
    {
        None
    } else {
        Some(window.bounds.covers(monitor))
    }
}

/// Windows must be ordered front to back. A regular app covering a fullscreen
/// window takes precedence, so background fullscreen windows don't hide the ticker.
pub fn detect(monitor: Bounds, windows: impl IntoIterator<Item = WindowState>) -> bool {
    windows
        .into_iter()
        .find_map(|window| classify(monitor, window))
        .unwrap_or(false)
}

/// # Safety
/// `monitor` must be a monitor handle obtained from the Windows monitor APIs.
pub unsafe fn on_monitor(monitor: HMONITOR) -> bool {
    struct Scan {
        monitor: Bounds,
        process: u32,
        fullscreen: bool,
    }
    unsafe extern "system" fn visit(hwnd: HWND, data: LPARAM) -> i32 {
        unsafe {
            let scan = &mut *(data as *mut Scan);
            if IsWindowVisible(hwnd) == 0 || IsIconic(hwnd) != 0 {
                return 1;
            }
            let mut process = 0;
            GetWindowThreadProcessId(hwnd, &mut process);
            if process == scan.process {
                return 1;
            }
            let ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE) as u32;
            if ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) != 0 {
                return 1;
            }
            let mut class = [0u16; 128];
            let len = GetClassNameW(hwnd, class.as_mut_ptr(), class.len() as i32);
            let class = String::from_utf16_lossy(&class[..len.max(0) as usize]);
            if class.starts_with("CryptoTaskbar.")
                || [
                    "Shell_TrayWnd",
                    "Shell_SecondaryTrayWnd",
                    "WorkerW",
                    "Progman",
                    "#32768",
                ]
                .contains(&class.as_str())
            {
                return 1;
            }
            let mut cloaked = 0u32;
            DwmGetWindowAttribute(
                hwnd,
                DWMWA_CLOAKED as u32,
                &mut cloaked as *mut _ as _,
                std::mem::size_of_val(&cloaked) as u32,
            );
            if cloaked != 0 {
                return 1;
            }
            let mut rect: RECT = std::mem::zeroed();
            // Visible bounds exclude the invisible resize borders of maximized apps.
            if DwmGetWindowAttribute(
                hwnd,
                DWMWA_EXTENDED_FRAME_BOUNDS as u32,
                &mut rect as *mut _ as _,
                std::mem::size_of_val(&rect) as u32,
            ) != 0
                && GetWindowRect(hwnd, &mut rect) == 0
            {
                return 1;
            }
            if let Some(fullscreen) = classify(
                scan.monitor,
                WindowState {
                    bounds: rect.into(),
                    visible: true,
                    minimized: false,
                    cloaked: false,
                    auxiliary: false,
                },
            ) {
                scan.fullscreen = fullscreen;
                return 0;
            }
            1
        }
    }
    unsafe {
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        if GetMonitorInfoW(monitor, &mut info) == 0 {
            return false;
        }
        let mut scan = Scan {
            monitor: info.rcMonitor.into(),
            process: GetCurrentProcessId(),
            fullscreen: false,
        };
        EnumWindows(Some(visit), &mut scan as *mut _ as LPARAM);
        scan.fullscreen
    }
}
