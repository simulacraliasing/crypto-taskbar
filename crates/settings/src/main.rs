#![cfg_attr(windows, windows_subsystem = "windows")]
mod platform;
mod ui;
use crypto_taskbar_core::{Config, default_config_path};
use gpui_kit::*;

fn main() {
    if let Err(error) = start() {
        platform::error(&format!("{error:#}"));
        std::process::exit(1);
    }
}
fn start() -> anyhow::Result<()> {
    let mut args = std::env::args_os().skip(1);
    let mut path = None;
    let mut inspect = false;
    let mut notify_class = "CryptoTaskbar.Window.v1".to_owned();
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--inspect") => inspect = true,
            Some("--config") => {
                path = Some(std::path::PathBuf::from(
                    args.next()
                        .ok_or_else(|| anyhow::anyhow!("--config 需要文件路径"))?,
                ))
            }
            Some("--notify-class") => {
                notify_class = args
                    .next()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .into_owned()
            }
            _ => {}
        }
    }
    let path = path.unwrap_or(default_config_path()?);
    let config = if path.exists() {
        Config::load(&path)?
    } else {
        let c = Config::default();
        c.save(&path)?;
        c
    };
    gpui_kit::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(move |cx| {
            gpui_kit::init(cx);
            cx.on_window_closed(|cx, _| {
                if cx.windows().is_empty() {
                    cx.quit();
                }
            })
            .detach();
            let screen = cx
                .primary_display()
                .map(|display| display.bounds().size)
                .unwrap_or(size(px(1024.), px(768.)));
            let dimensions = size(
                px(840.).min((screen.width - px(48.)).max(px(600.))),
                px(680.).min((screen.height - px(80.)).max(px(460.))),
            );
            let bounds = Bounds::centered(None, dimensions, cx);
            let options = WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                window_min_size: Some(size(px(600.), px(460.))),
                titlebar: Some(TitlebarOptions {
                    title: Some("Crypto Taskbar — 配置".into()),
                    ..Default::default()
                }),
                show: false,
                focus: false,
                is_minimizable: false,
                ..Default::default()
            };
            if let Err(error) = gpui_kit::open_window(options, cx, move |window, cx| {
                let view = cx.new(|cx| ui::Settings::new(config, path, notify_class, window, cx));
                platform::show_window(window, inspect);
                view
            }) {
                platform::error(&error.to_string());
                cx.quit();
            }
        });
    Ok(())
}
