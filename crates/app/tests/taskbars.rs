#![cfg(windows)]
use crypto_taskbar::taskbars::{Rect, place};
fn rect(left: i32, top: i32, right: i32, bottom: i32) -> Rect {
    Rect {
        left,
        top,
        right,
        bottom,
    }
}
#[test]
fn independent_placement_for_mixed_dpi_and_negative_coordinates() {
    assert_eq!(
        place(
            rect(0, 2088, 3840, 2160),
            rect(0, 0, 3840, 2160),
            144,
            150,
            16,
            None
        ),
        Some(rect(24, 2089, 249, 2158))
    );
    assert_eq!(
        place(
            rect(-2880, 1704, 0, 1800),
            rect(-2880, 0, 0, 1800),
            192,
            150,
            16,
            None
        ),
        Some(rect(-2848, 1706, -2548, 1798))
    );
}
#[test]
fn right_position_is_anchored_to_each_taskbars_tray() {
    assert_eq!(
        place(
            rect(-2880, 1704, 0, 1800),
            rect(-2880, 0, 0, 1800),
            192,
            150,
            16,
            Some(-240)
        ),
        Some(rect(-572, 1706, -272, 1798))
    );
}
#[test]
fn oversized_width_and_offset_stay_within_the_taskbar() {
    assert_eq!(
        place(
            rect(100, 950, 900, 1000),
            rect(100, 0, 900, 1000),
            192,
            1200,
            10000,
            None
        ),
        Some(rect(100, 950, 900, 1000))
    );
    assert_eq!(
        place(
            rect(100, 950, 900, 1000),
            rect(100, 0, 900, 1000),
            96,
            150,
            10000,
            Some(150)
        ),
        Some(rect(100, 952, 250, 998))
    );
}
#[test]
fn hidden_or_unsupported_taskbar_only_suspends_its_own_display() {
    let left = place(
        rect(-2880, 1799, 0, 1801),
        rect(-2880, 0, 0, 1800),
        192,
        150,
        16,
        None,
    );
    let primary = place(
        rect(0, 2088, 3840, 2160),
        rect(0, 0, 3840, 2160),
        144,
        150,
        16,
        None,
    );
    assert!(left.is_none());
    assert!(primary.is_some());
    assert!(
        place(
            rect(0, 0, 48, 1080),
            rect(0, 0, 1920, 1080),
            96,
            150,
            16,
            None
        )
        .is_none()
    );
    assert!(
        place(
            rect(0, 0, 1920, 48),
            rect(0, 0, 1920, 1080),
            96,
            150,
            16,
            None
        )
        .is_none()
    );
}
