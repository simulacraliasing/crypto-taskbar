fn main() {
    println!("cargo:rerun-if-changed=../../resources/app.ico");
    println!("cargo:rerun-if-changed=../../resources/app.manifest");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        winresource::WindowsResource::new()
            .set_icon("../../resources/app.ico")
            .set_manifest_file("../../resources/app.manifest")
            .set("ProductName", "Crypto Taskbar")
            .set("FileDescription", "Crypto Taskbar")
            .compile()
            .expect("compile Windows resources");
    }
}
