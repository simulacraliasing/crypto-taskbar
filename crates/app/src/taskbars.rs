//! Taskbar discovery and per-monitor placement in physical pixels.
use windows_sys::Win32::{Foundation::*, Graphics::Gdi::*, UI::WindowsAndMessaging::*};

pub fn discover() -> Vec<HWND> {
    unsafe extern "system" fn visit(hwnd: HWND, data: LPARAM) -> i32 {
        unsafe {
            let taskbars = &mut *(data as *mut Vec<HWND>);
            let mut class = [0u16; 64];
            let len = GetClassNameW(hwnd, class.as_mut_ptr(), class.len() as i32);
            let class = String::from_utf16_lossy(&class[..len.max(0) as usize]);
            if class == "Shell_TrayWnd" {
                taskbars.insert(0, hwnd);
            } else if class == "Shell_SecondaryTrayWnd" {
                taskbars.push(hwnd);
            }
            1
        }
    }
    let mut taskbars: Vec<HWND> = Vec::new();
    unsafe {
        EnumWindows(Some(visit), &mut taskbars as *mut _ as LPARAM);
    }
    let mut monitors = Vec::new();
    taskbars.retain(|hwnd| {
        let monitor = unsafe { MonitorFromWindow(*hwnd, MONITOR_DEFAULTTONEAREST) };
        if monitors.contains(&monitor) {
            false
        } else {
            monitors.push(monitor);
            true
        }
    });
    taskbars
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}
impl From<RECT> for Rect {
    fn from(r: RECT) -> Self {
        Self {
            left: r.left,
            top: r.top,
            right: r.right,
            bottom: r.bottom,
        }
    }
}

/// Returns None for an offscreen/auto-hidden bar or a non-bottom taskbar.
pub fn place(
    bar: Rect,
    monitor: Rect,
    dpi: u32,
    width: i32,
    offset: i32,
    right: Option<i32>,
) -> Option<Rect> {
    let scale = (dpi as f64 / 96.).max(1.);
    let px = |n: i32| (n as f64 * scale).round() as i32;
    let bar_width = bar.right - bar.left;
    let bar_height = bar.bottom - bar.top;
    if bar_width <= 0
        || bar_height <= 2
        || bar_height > px(100)
        || bar.top < monitor.top + (monitor.bottom - monitor.top) / 2
        || bar.top >= monitor.bottom - 2
    {
        return None;
    }
    let width = px(width).min(bar_width);
    let height = px(46).min(bar_height);
    let x = right
        .map(|r| r - px(offset) - width)
        .unwrap_or(bar.left + px(offset))
        .clamp(bar.left, (bar.right - width).max(bar.left));
    let y = bar.top + (bar_height - height) / 2;
    Some(Rect {
        left: x,
        top: y,
        right: x + width,
        bottom: y + height,
    })
}
