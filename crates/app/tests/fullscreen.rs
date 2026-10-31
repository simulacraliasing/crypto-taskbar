#![cfg(windows)]
use crypto_taskbar::fullscreen::{Bounds, WindowState, detect};

fn bounds(left: i32, top: i32, right: i32, bottom: i32) -> Bounds {
    Bounds {
        left,
        top,
        right,
        bottom,
    }
}
fn window(bounds: Bounds) -> WindowState {
    WindowState {
        bounds,
        visible: true,
        minimized: false,
        cloaked: false,
        auxiliary: false,
    }
}

#[test]
fn primary_video_stays_fullscreen_when_other_monitor_has_focus() {
    // Actual failing layout: 4K/150% primary and a 2880x1800/200% display to its left.
    let primary = bounds(0, 0, 3840, 2160);
    let foreground_on_left = window(bounds(-2880, 0, 0, 1704));
    let video_on_primary = window(primary);
    assert!(detect(primary, [foreground_on_left, video_on_primary]));
    assert!(!detect(
        bounds(-2880, 0, 0, 1800),
        [foreground_on_left, video_on_primary]
    ));
}

#[test]
fn exiting_fullscreen_restores_ticker_for_maximized_browser() {
    let screen = bounds(0, 0, 3840, 2160);
    assert!(detect(screen, [window(screen)]));
    assert!(!detect(screen, [window(bounds(0, 0, 3840, 2088))]));
    assert!(!detect(screen, []));
}

#[test]
fn covered_background_fullscreen_window_does_not_hide_ticker() {
    let screen = bounds(0, 0, 1920, 1080);
    assert!(!detect(
        screen,
        [window(bounds(100, 100, 900, 700)), window(screen)]
    ));
}

#[test]
fn taskbar_and_transient_windows_do_not_mask_video() {
    let screen = bounds(0, 0, 1920, 1080);
    let taskbar = WindowState {
        auxiliary: true,
        ..window(bounds(0, 1032, 1920, 1080))
    };
    let tooltip = WindowState {
        auxiliary: true,
        ..window(bounds(100, 100, 200, 200))
    };
    assert!(detect(screen, [taskbar, tooltip, window(screen)]));
}

#[test]
fn minimized_hidden_and_other_desktop_windows_are_ignored() {
    let screen = bounds(0, 0, 1920, 1080);
    for ignored in [
        WindowState {
            minimized: true,
            ..window(screen)
        },
        WindowState {
            visible: false,
            ..window(screen)
        },
        WindowState {
            cloaked: true,
            ..window(screen)
        },
    ] {
        assert!(!detect(screen, [ignored]));
        assert!(!detect(
            screen,
            [ignored, window(bounds(100, 100, 900, 700))]
        ));
    }
}

#[test]
fn only_small_dpi_rounding_error_is_tolerated() {
    let screen = bounds(-2880, -1800, 0, 0);
    assert!(detect(screen, [window(bounds(-2879, -1799, -1, -1))]));
    assert!(!detect(screen, [window(bounds(-2880, -1800, 0, -96))]));
    assert!(!detect(screen, [window(bounds(0, 0, 3840, 2160))]));
}
