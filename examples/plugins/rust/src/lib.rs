//! Example oshot plugin in Rust: uppercases the OCR output. It does what the C
//! example (../c/uppercase.c) does. src/ffi.rs has the bindings to oshot's
//! plugin API, this file keeps the `unsafe` in a few small wrappers.
//!
//! - render(): a button that uppercases the text, and the selection's size
//! - on_ocr_done(): optionally uppercases new text right away
//! - settings: that option and a prefix, which oshot draws in Preferences >
//!   Plugins and saves in the plugin's config.toml
//!
//! It draws only with the host's ui_* functions (imgui_version = 0), so it
//! needs neither cimgui nor oshot's SDK to build. Unlike the C example it
//! uppercases all of Unicode, not just ASCII.
//!
//! Build it with `cargo build --release`, or install the folder with
//! `oshotpm install <this folder>`. See docs/plugins.md. Public domain
//! (Unlicense).

mod ffi;

use std::ffi::{CStr, c_void};

use ffi::*;

type State = *mut c_void;

/// oshot draws these in Preferences > Plugins and saves them in config.toml
static SETTINGS: [oshot_setting_t; 2] = [
    oshot_setting_t::new(
        OSHOT_SETTING_BOOL,
        c"auto-uppercase",
        c"Uppercase as soon as text is extracted",
        oshot_value_t { kind: OSHOT_VAL_BOOL, v: oshot_value_u { b: false } },
    ),
    oshot_setting_t {
        help: c"Put before the uppercased text".as_ptr(),
        ..oshot_setting_t::new(
            OSHOT_SETTING_STRING,
            c"prefix",
            c"Prefix",
            oshot_value_t { kind: OSHOT_VAL_STRING, v: oshot_value_u { s: oshot_str_t::EMPTY } },
        )
    },
];

/// A string oshot returned, freed with host->str_free when dropped
struct HostString<'a> {
    host: &'a oshot_host_t,
    s: oshot_str_t,
}

impl HostString<'_> {
    fn as_bytes(&self) -> &[u8] {
        // SAFETY: strings from oshot are valid until str_free
        unsafe { self.s.as_bytes() }
    }
}

impl Drop for HostString<'_> {
    fn drop(&mut self) {
        unsafe { host_fn!(self.host, str_free)(&mut self.s) }
    }
}

struct Plugin {
    host: &'static oshot_host_t, // oshot's table outlives the plugin
    ctx: *mut oshot_ctx_t,

    // The settings, as last saved
    auto_uppercase: bool, // uppercase as soon as OCR is done
    prefix: String,       // put before the uppercased text

    capture_size: Option<(i32, i32)>, // size of the selection when "Uppercase" was last pressed
}

// The safe wrappers around the host calls this plugin uses
impl Plugin {
    fn log(&self, level: oshot_log_level_t, msg: &str) {
        unsafe { host_fn!(self.host, log)(self.ctx, level, oshot_str_t::borrow(msg.as_bytes())) }
    }

    fn get_bool(&self, key: &CStr, fallback: bool) -> bool {
        unsafe { host_fn!(self.host, get_bool)(self.ctx, OSHOT_STORE_CONFIG, key.as_ptr(), fallback) }
    }

    fn get_string(&self, key: &CStr, fallback: &[u8]) -> HostString<'static> {
        let s = unsafe {
            host_fn!(self.host, get_string)(self.ctx, OSHOT_STORE_CONFIG, key.as_ptr(), oshot_str_t::borrow(fallback))
        };
        HostString { host: self.host, s }
    }

    fn get_text(&self, id: &CStr) -> Option<HostString<'static>> {
        let mut s = oshot_str_t::EMPTY;
        unsafe { host_fn!(self.host, get_text)(self.ctx, id.as_ptr(), &mut s) }
            .then_some(HostString { host: self.host, s })
    }

    fn set_text(&self, id: &CStr, val: &[u8]) {
        unsafe { host_fn!(self.host, set_text)(self.ctx, id.as_ptr(), oshot_str_t::borrow(val)) };
    }

    /// Width and height of the selection. get_capture() copies the whole
    /// image, so only on demand, never every frame.
    fn capture_size(&self) -> (i32, i32) {
        unsafe {
            let mut cap = host_fn!(self.host, get_capture)(self.ctx);
            let size = (cap.w, cap.h);
            host_fn!(self.host, capture_free)(&mut cap);
            size
        }
    }

    fn ui_button(&self, label: &str) -> bool {
        unsafe { host_fn!(self.host, ui_button)(self.ctx, oshot_str_t::borrow(label.as_bytes())) }
    }

    fn ui_text(&self, text: &str, flags: u32) {
        unsafe { host_fn!(self.host, ui_text)(self.ctx, oshot_str_t::borrow(text.as_bytes()), flags) }
    }
}

// What the plugin does
impl Plugin {
    fn new(host: &'static oshot_host_t, ctx: *mut oshot_ctx_t) -> Self {
        let mut plugin = Plugin { host, ctx, auto_uppercase: false, prefix: String::new(), capture_size: None };
        plugin.read_settings();
        plugin.log(OSHOT_LOG_INFO, "uppercase plugin (Rust) loaded");
        plugin
    }

    /// Reads the settings with the same keys and fallbacks as SETTINGS
    fn read_settings(&mut self) {
        self.auto_uppercase = self.get_bool(c"auto-uppercase", false);
        self.prefix = String::from_utf8_lossy(self.get_string(c"prefix", b"").as_bytes()).into_owned();
    }

    /// Replaces oshot's OCR output with prefix + UPPERCASE(text)
    fn uppercase_text(&self, text: &[u8]) {
        let out = self.prefix.clone() + &String::from_utf8_lossy(text).to_uppercase();
        self.set_text(OSHOT_TEXT_OCR, out.as_bytes());
    }

    fn render(&mut self) {
        if self.ui_button("Uppercase the OCR output") {
            if let Some(text) = self.get_text(OSHOT_TEXT_OCR) {
                self.uppercase_text(text.as_bytes());
            }
            self.capture_size = Some(self.capture_size());
        }

        if let Some((w, h)) = self.capture_size {
            self.ui_text(&format!("Selection: {w}x{h}"), OSHOT_UI_DISABLED);
        }
    }

    fn on_ocr_done(&self, result: &oshot_ocr_result_t) {
        // Empty when nothing was read
        if self.auto_uppercase && result.text.len > 0 {
            // SAFETY: the text is valid during on_ocr_done()
            self.uppercase_text(unsafe { result.text.as_bytes() });
        }
    }
}

// The C callbacks. A panic can't unwind into oshot: Rust aborts instead, so
// oshot exits right there rather than continuing in a broken state.

unsafe extern "C" fn plugin_init(host: *const oshot_host_t, ctx: *mut oshot_ctx_t, state: *mut State) -> bool {
    // SAFETY: oshot's host table lives as long as the process
    let host = unsafe { &*host };

    // The ui_* functions came after the first plugin API: check the last one
    if !host_has!(host, ui_end_section) {
        let msg = b"this oshot is too old for this plugin: update it";
        unsafe { host_fn!(host, log)(ctx, OSHOT_LOG_ERROR, oshot_str_t::borrow(msg)) };
        return false;
    }

    let plugin = Box::new(Plugin::new(host, ctx));
    unsafe { *state = Box::into_raw(plugin).cast() };
    true
}

/// # Safety
/// `state` is the pointer plugin_init() stored, which oshot passes to every callback
unsafe fn plugin<'a>(state: State) -> &'a mut Plugin {
    unsafe { &mut *state.cast::<Plugin>() }
}

unsafe extern "C" fn plugin_destroy(state: State) {
    drop(unsafe { Box::from_raw(state.cast::<Plugin>()) });
}

unsafe extern "C" fn plugin_render(state: State) {
    unsafe { plugin(state) }.render();
}

unsafe extern "C" fn plugin_on_ocr_done(state: State, result: *const oshot_ocr_result_t) {
    unsafe { plugin(state).on_ocr_done(&*result) };
}

unsafe extern "C" fn plugin_on_settings_saved(state: State) {
    unsafe { plugin(state) }.read_settings();
}

static DESCRIPTOR: oshot_plugin_t = oshot_plugin_t {
    struct_size: size_of::<oshot_plugin_t>() as u32,
    abi_version: OSHOT_API_VERSION,
    imgui_version: 0, // no cimgui
    id: c"io.github.toni500github.uppercase-rust".as_ptr(),
    name: c"Uppercase (Rust)".as_ptr(),
    init: Some(plugin_init),
    destroy: Some(plugin_destroy),
    render: Some(plugin_render),
    on_ocr_done: Some(plugin_on_ocr_done),
    render_preferences: None,
    on_save_preferences: None,
    on_discard_preferences: None,
    settings: SETTINGS.as_ptr(),
    n_settings: SETTINGS.len(),
    on_settings_saved: Some(plugin_on_settings_saved),
    on_barcodes_done: None,
    on_capture_start: None,
    on_capture_end: None,
    on_selection_changed: None,
};

/// The one symbol the library exports
#[unsafe(no_mangle)]
pub extern "C" fn oshot_plugin_entry() -> *const oshot_plugin_t {
    &DESCRIPTOR
}
