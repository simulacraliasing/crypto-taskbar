//! Read-only window geometry diagnostics; no titles or page contents are read.
use windows_sys::Win32::{
    Foundation::*,
    Graphics::{Dwm::*, Gdi::*},
    UI::{HiDpi::*, WindowsAndMessaging::*},
};
unsafe extern "system" fn inspect(hwnd: HWND, _: LPARAM) -> i32 {
    unsafe {
        let mut text = [0u16; 128];
        let len = GetClassNameW(hwnd, text.as_mut_ptr(), text.len() as i32);
        let class = String::from_utf16_lossy(&text[..len.max(0) as usize]);
        if IsWindowVisible(hwnd) == 0 && !class.starts_with("CryptoTaskbar") {
            return 1;
        }
        let mut process = 0;
        GetWindowThreadProcessId(hwnd, &mut process);
        let foreground = hwnd == GetForegroundWindow();
        if !foreground
            && !class.starts_with("Chrome_")
            && !class.starts_with("CryptoTaskbar")
            && class != "Shell_TrayWnd"
        {
            return 1;
        }
        let mut rect: RECT = std::mem::zeroed();
        GetWindowRect(hwnd, &mut rect);
        let mut frame: RECT = std::mem::zeroed();
        let hr = DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS as u32,
            &mut frame as *mut _ as _,
            std::mem::size_of::<RECT>() as u32,
        );
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mut info);
        let r = |v: RECT| [v.left, v.top, v.right, v.bottom];
        println!(
            "{}",
            serde_json::json!({"class":class,"process":process,"visible":IsWindowVisible(hwnd)!=0,"foreground":foreground,"minimized":IsIconic(hwnd)!=0,"dpi":GetDpiForWindow(hwnd),"rect":r(rect),"frame":if hr==0{Some(r(frame))}else{None},"monitor":r(info.rcMonitor),"work":r(info.rcWork),"style":GetWindowLongW(hwnd,GWL_STYLE) as u32,"exStyle":GetWindowLongW(hwnd,GWL_EXSTYLE) as u32})
        );
        1
    }
}
fn main() {
    unsafe {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        EnumWindows(Some(inspect), 0);
        let class: Vec<u16> = "Shell_TrayWnd\0".encode_utf16().collect();
        let taskbar = FindWindowW(class.as_ptr(), std::ptr::null());
        let monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY);
        println!(
            "fullscreen_on_taskbar_monitor={}",
            crypto_taskbar::fullscreen::on_monitor(monitor)
        );
    }
}
