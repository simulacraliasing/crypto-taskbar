use gpui_kit::Window;

pub fn quit(cx: &mut gpui_kit::App) {
    use std::sync::atomic::{AtomicBool, Ordering};
    static EXITING: AtomicBool = AtomicBool::new(false);
    if !EXITING.swap(true, Ordering::SeqCst) {
        // Saving is synchronous and complete before this is called. Some Windows
        // GPU backends can stall on shutdown after the last window disappears;
        // don't leave the settings helper alive and prevent it being reopened.
        std::thread::spawn(|| {
            std::thread::sleep(std::time::Duration::from_secs(2));
            std::process::exit(0);
        });
    }
    cx.quit();
}
#[cfg(windows)]
fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}
pub fn show_window(window: &mut Window, inspect: bool) {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::UI::WindowsAndMessaging::*;
        if let Ok(handle) = raw_window_handle::HasWindowHandle::window_handle(window)
            && let raw_window_handle::RawWindowHandle::Win32(handle) = handle.as_raw()
        {
            let hwnd = handle.hwnd.get() as windows_sys::Win32::Foundation::HWND;
            let style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(
                hwnd,
                GWL_EXSTYLE,
                if inspect {
                    (style & !(WS_EX_TOOLWINDOW as isize)) | WS_EX_APPWINDOW as isize
                } else {
                    (style & !(WS_EX_APPWINDOW as isize)) | WS_EX_TOOLWINDOW as isize
                },
            );
            SetWindowPos(
                hwnd,
                std::ptr::null_mut(),
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED,
            );
        }
    }
    // Let GPUI apply its saved initial bounds and initialize the render surface.
    window.activate_window();
    window.refresh();
}
pub fn reload(class: &str) {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::UI::WindowsAndMessaging::*;
        let hwnd = FindWindowW(wide(class).as_ptr(), std::ptr::null());
        if !hwnd.is_null() {
            let msg = RegisterWindowMessageW(wide("CryptoTaskbar.ReloadConfig.v2").as_ptr());
            PostMessageW(hwnd, msg, 0, 0);
        }
    }
}
pub fn titlebar_theme(window: &Window, dark: bool) {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::Graphics::Dwm::{
            DWMWA_USE_IMMERSIVE_DARK_MODE, DwmSetWindowAttribute,
        };
        if let Ok(handle) = raw_window_handle::HasWindowHandle::window_handle(window)
            && let raw_window_handle::RawWindowHandle::Win32(handle) = handle.as_raw()
        {
            let value = i32::from(dark);
            DwmSetWindowAttribute(
                handle.hwnd.get() as _,
                DWMWA_USE_IMMERSIVE_DARK_MODE as u32,
                &value as *const _ as _,
                std::mem::size_of_val(&value) as u32,
            );
        }
    }
}
pub fn system_dark() -> bool {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::System::Registry::*;
        let mut value = 0u32;
        let mut size = 4;
        RegGetValueW(
            HKEY_CURRENT_USER,
            wide("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize").as_ptr(),
            wide("SystemUsesLightTheme").as_ptr(),
            RRF_RT_REG_DWORD,
            std::ptr::null_mut(),
            &mut value as *mut _ as *mut std::ffi::c_void,
            &mut size,
        );
        value == 0
    }
    #[cfg(not(windows))]
    false
}
pub fn error(message: &str) {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::UI::WindowsAndMessaging::*;
        MessageBoxW(
            std::ptr::null_mut(),
            wide(message).as_ptr(),
            wide("Crypto Taskbar").as_ptr(),
            MB_OK | MB_ICONERROR,
        );
    }
    #[cfg(not(windows))]
    eprintln!("{message}");
}
