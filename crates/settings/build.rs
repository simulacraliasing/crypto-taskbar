fn main() {
    println!("cargo:rerun-if-changed=../../resources/app.ico");
    // GPUI already embeds a PerMonitorV2 manifest; a second ID 1 conflicts.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        winresource::WindowsResource::new()
            .set_icon("../../resources/app.ico")
            .set("ProductName", "Crypto Taskbar")
            .set("FileDescription", "Crypto Taskbar Settings")
            .compile()
            .expect("compile Windows resources");
    }
}
