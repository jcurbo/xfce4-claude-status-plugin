use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let output_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let header_path = output_dir.join("../../../claude_status_core.h");

    cbindgen::Builder::new()
        .with_crate(crate_dir)
        .with_language(cbindgen::Language::C)
        .with_include_guard("CLAUDE_STATUS_CORE_H")
        .with_documentation(true)
        .generate()
        .expect("Unable to generate C bindings")
        .write_to_file(header_path);
}
