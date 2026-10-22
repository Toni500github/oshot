//! Generates the bindings to oshot's SDK and sets up linking with cimgui.
//!
//! oshotpm sets the OSHOT_SDK environment variable when it builds plugins; set
//! it yourself to build with cargo directly, e.g.
//!     OSHOT_SDK=<oshot>/build/release/sdk cargo build --release
//!
//! bindgen needs libclang: https://rust-lang.github.io/rust-bindgen/requirements.html

use std::{env, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=OSHOT_SDK");
    let sdk = PathBuf::from(
        env::var_os("OSHOT_SDK")
            .expect("oshot's plugin SDK wasn't found: set OSHOT_SDK to the folder with include/oshot/oshot_plugin.h"),
    );
    let include = sdk.join("include");
    for header in ["oshot_plugin.h", "cimgui.h"] {
        println!("cargo:rerun-if-changed={}", include.join("oshot").join(header).display());
    }

    // Generated from the SDK of the oshot the plugin is built for, so the
    // bindings always match its plugin API and its Dear ImGui version, which a
    // plugin must be built with exactly. All of cimgui is in them.
    let bindings = bindgen::Builder::default()
        .header_contents(
            "oshot.h",
            "#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS\n\
             #include <oshot/cimgui.h>\n\
             #include <oshot/oshot_plugin.h>\n",
        )
        .clang_arg(format!("-I{}", include.display()))
        .allowlist_file(r".*oshot[/\\](oshot_plugin|cimgui)\.h")
        .rust_target(bindgen::RustTarget::stable(85, 0).unwrap()) // the crate's rust-version
        .prepend_enum_name(false) // OSHOT_LOG_INFO, not oshot_log_level_t_OSHOT_LOG_INFO
        .generate_cstr(true) // OSHOT_TEXT_OCR as a &CStr
        .generate()
        .expect("can't generate the bindings to oshot's SDK");

    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("oshot.rs");
    bindings.write_to_file(&out).unwrap_or_else(|e| panic!("can't write {}: {e}", out.display()));

    // cimgui comes from oshot: Linux and the BSDs resolve it when oshot loads
    // the plugin, macOS needs to be told to, Windows links its import library
    match env::var("CARGO_CFG_TARGET_OS").unwrap().as_str() {
        "windows" => {
            println!("cargo:rustc-link-search=native={}", sdk.join("lib").display());
            println!("cargo:rustc-link-lib=dylib=cimgui");
        }
        "macos" => println!("cargo:rustc-cdylib-link-arg=-Wl,-undefined,dynamic_lookup"),
        _ => {}
    }
}
