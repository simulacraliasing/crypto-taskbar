#![cfg_attr(windows, windows_subsystem = "windows")]
#[cfg(windows)]
mod native;
#[cfg(windows)]
mod paint;
#[cfg(windows)]
fn main() {
    if let Err(error) = start() {
        native::message(&format!("{error:#}"));
        std::process::exit(1);
    }
}
#[cfg(not(windows))]
fn main() {
    eprintln!("Crypto Taskbar requires Windows.");
}
#[cfg(windows)]
fn start() -> anyhow::Result<()> {
    let mut args = std::env::args_os().skip(1);
    let mut path = None;
    let mut settings = false;
    let mut preview = None;
    let mut test = false;
    let mut inspect = false;
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--inspect") => {
                inspect = true;
                test = true;
            }
            Some("--config") => {
                path = Some(std::path::PathBuf::from(
                    args.next()
                        .ok_or_else(|| anyhow::anyhow!("--config 需要文件路径"))?,
                ))
            }
            Some("--settings") => settings = true,
            Some("--preview-light") => preview = Some(false),
            Some("--preview-dark") => preview = Some(true),
            Some("--test-instance") => test = true,
            _ => {}
        }
    }
    native::run(native::Options {
        path: path.unwrap_or(crypto_taskbar_core::default_config_path()?),
        settings,
        preview,
        test,
        inspect,
    })
}
