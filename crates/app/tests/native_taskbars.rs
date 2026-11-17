//! Optional desktop integration check. Uses only its own ticker and fixture windows.
#![cfg(windows)]
use crypto_taskbar_core::{Config, Source};
use std::{
    io::{Read, Write},
    net::TcpListener,
    process::{Child, Command},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use windows_sys::Win32::{
    Foundation::*,
    Graphics::Gdi::*,
    UI::{HiDpi::*, WindowsAndMessaging::*},
};
fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}
struct Process(Child);
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
struct Fixture(HWND);
impl Drop for Fixture {
    fn drop(&mut self) {
        unsafe {
            DestroyWindow(self.0);
        }
    }
}
#[derive(Clone, Copy)]
struct Window {
    hwnd: HWND,
    overlay: bool,
    monitor: HMONITOR,
}
fn windows(pid: u32) -> Vec<Window> {
    struct Scan {
        pid: u32,
        found: Vec<Window>,
    }
    unsafe extern "system" fn visit(hwnd: HWND, data: LPARAM) -> i32 {
        unsafe {
            let scan = &mut *(data as *mut Scan);
            let mut pid = 0;
            GetWindowThreadProcessId(hwnd, &mut pid);
            if pid == scan.pid {
                let mut class = [0u16; 64];
                let class_len = GetClassNameW(hwnd, class.as_mut_ptr(), 64);
                if String::from_utf16_lossy(&class[..class_len.max(0) as usize])
                    != "CryptoTaskbar.RustTest"
                {
                    return 1;
                }
                let mut title = [0u16; 128];
                let len = GetWindowTextW(hwnd, title.as_mut_ptr(), 128);
                let title = String::from_utf16_lossy(&title[..len.max(0) as usize]);
                if title != "CryptoTaskbar.Controller" {
                    scan.found.push(Window {
                        hwnd,
                        overlay: title == "Crypto Taskbar",
                        monitor: MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY),
                    });
                }
            }
            1
        }
    }
    let mut scan = Scan {
        pid,
        found: Vec::new(),
    };
    unsafe {
        EnumWindows(Some(visit), &mut scan as *mut _ as LPARAM);
    }
    scan.found
}
fn wait_for(mut condition: impl FnMut() -> bool) {
    let start = Instant::now();
    while !condition() {
        assert!(
            start.elapsed() < Duration::from_secs(8),
            "Desktop condition timed out"
        );
        thread::sleep(Duration::from_millis(50));
    }
}

#[test]
#[ignore = "requires an interactive Windows desktop; briefly shows a test fullscreen window"]
fn displays_share_requests_reload_and_resume_after_fullscreen() {
    unsafe {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    let taskbars = crypto_taskbar::taskbars::discover();
    assert!(!taskbars.is_empty(), "Windows taskbar required");
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let endpoint = format!("http://{}/quotes", listener.local_addr().unwrap());
    listener.set_nonblocking(true).unwrap();
    let requests = Arc::new(AtomicUsize::new(0));
    let count = requests.clone();
    let stop = Arc::new(AtomicBool::new(false));
    let stopping = stop.clone();
    let server = thread::spawn(move || {
        while !stopping.load(Ordering::Relaxed) {
            if let Ok((mut stream, _)) = listener.accept() {
                let _ = stream.set_read_timeout(Some(Duration::from_secs(1)));
                let mut request = [0u8; 8192];
                let _ = stream.read(&mut request);
                count.fetch_add(1, Ordering::SeqCst);
                let body = r#"{"BTCUSDT":{"price":100,"change":2},"ETHUSDT":{"price":200,"change":-1},"SOLUSDT":{"price":300,"change":3}}"#;
                let _ = write!(
                    stream,
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    body.len(),
                    body
                );
            } else {
                thread::sleep(Duration::from_millis(10));
            }
        }
    });
    let root = std::env::var_os("CT_TEST_DIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join(format!(
            "native-taskbars-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
    let path = root.join("config.json");
    let mut config = Config {
        refresh_seconds: 60,
        offset: 300,
        ..Config::default()
    };
    config.sources.clear();
    config.sources.insert(
        "test".into(),
        Source {
            url: endpoint,
            price_path: "/{symbol}/price".into(),
            change_path: "/{symbol}/change".into(),
            ..Source::default()
        },
    );
    for coin in &mut config.coins {
        coin.source = "test".into();
    }
    config.save(&path).unwrap();
    let mut child = Process(
        Command::new(env!("CARGO_BIN_EXE_CryptoTaskbar"))
            .arg("--test-instance")
            .arg("--config")
            .arg(&path)
            .spawn()
            .unwrap(),
    );
    eprintln!("child={} taskbars={}", child.0.id(), taskbars.len());
    wait_for(|| windows(child.0.id()).len() == taskbars.len() * 2);
    eprintln!("windows created");
    wait_for(|| requests.load(Ordering::SeqCst) > 0);
    eprintln!("quote received");
    assert_eq!(
        requests.load(Ordering::SeqCst),
        1,
        "One batch request for all taskbars"
    );
    let all = windows(child.0.id());
    let overlays: Vec<_> = all
        .iter()
        .filter(|window| window.overlay)
        .copied()
        .collect();
    assert_eq!(overlays.len(), taskbars.len());
    let target = *overlays
        .iter()
        .find(|window| unsafe { IsWindowVisible(window.hwnd) != 0 })
        .expect("At least one visible bottom taskbar needed");
    unsafe {
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        assert_ne!(GetMonitorInfoW(target.monitor, &mut info), 0);
        let r = info.rcMonitor;
        let fixture = Fixture(CreateWindowExW(
            0,
            wide("STATIC").as_ptr(),
            wide("Crypto Taskbar fullscreen test").as_ptr(),
            WS_POPUP,
            r.left,
            r.top,
            r.right - r.left,
            r.bottom - r.top,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null(),
        ));
        assert!(!fixture.0.is_null());
        SetWindowPos(
            fixture.0,
            HWND_TOPMOST,
            r.left,
            r.top,
            r.right - r.left,
            r.bottom - r.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW,
        );
        wait_for(|| IsWindowVisible(target.hwnd) == 0);
        eprintln!("fullscreen hidden");
        assert!(
            all.iter()
                .filter(|window| window.monitor == target.monitor)
                .all(|window| IsWindowVisible(window.hwnd) == 0)
        );
        for other in overlays
            .iter()
            .filter(|window| window.monitor != target.monitor)
        {
            if !crypto_taskbar::fullscreen::on_monitor(other.monitor) {
                assert_ne!(IsWindowVisible(other.hwnd), 0);
            }
        }
        drop(fixture);
        wait_for(|| IsWindowVisible(target.hwnd) != 0);
        eprintln!("fullscreen restored");
        let controller = FindWindowW(
            wide("CryptoTaskbar.RustTest").as_ptr(),
            wide("CryptoTaskbar.Controller").as_ptr(),
        );
        assert!(!controller.is_null());
        PostMessageW(controller, WM_DISPLAYCHANGE, 0, 0);
        thread::sleep(Duration::from_millis(250));
        assert_eq!(
            windows(child.0.id()).len(),
            taskbars.len() * 2,
            "Discovery must not duplicate windows"
        );
        config.width = 210;
        config.save(&path).unwrap();
        let reload = RegisterWindowMessageW(wide("CryptoTaskbar.ReloadConfig.v2").as_ptr());
        PostMessageW(controller, reload, 0, 0);
        wait_for(|| {
            overlays.iter().all(|window| {
                let mut rect: RECT = std::mem::zeroed();
                GetWindowRect(window.hwnd, &mut rect);
                rect.right - rect.left
                    == (210. * GetDpiForWindow(window.hwnd) as f64 / 96.).round() as i32
            })
        });
        eprintln!("all displays reloaded");
        PostMessageW(controller, WM_CLOSE, 0, 0);
    }
    wait_for(|| child.0.try_wait().unwrap().is_some());
    stop.store(true, Ordering::Relaxed);
    server.join().unwrap();
    println!(
        "Verified {} taskbars: shared request, fullscreen hide/resume, reload, rediscovery, exit",
        taskbars.len()
    );
}
