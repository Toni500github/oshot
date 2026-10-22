//! Example oshot plugin in Rust: uppercases the OCR output. It does what the C
//! example (../c/uppercase.c) does. build.rs generates the bindings to oshot's
//! SDK with bindgen (src/ffi.rs), this file keeps the `unsafe` in a few small
//! wrappers.
//!
//! - render():             a button that uppercases the text, and the selection's size
//! - on_ocr_done():        optionally uppercases new text right away
//! - render_preferences(): a setting for that, plus a prefix, saved in the plugin's config.toml
//!
//! Unlike the C example it uppercases all of Unicode, not just ASCII.
//!
//! Build it with `cargo build --release` (see build.rs), or install the folder
//! with `oshotpm install <this folder>`. See docs/plugins.md. Public domain
//! (Unlicense).

mod ffi;

use std::ffi::{CStr, CString, c_char, c_void};

use ffi::*;

type State = *mut c_void;

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

#[derive(Clone, PartialEq)]
struct Prefs {
    auto_uppercase: bool, // uppercase as soon as OCR is done
    prefix: [u8; 64],     // put before the uppercased text; a NUL-terminated buffer for igInputText
}

impl Prefs {
    fn prefix(&self) -> &[u8] {
        CStr::from_bytes_until_nul(&self.prefix).map_or(&[], CStr::to_bytes)
    }

    fn set_prefix(&mut self, s: &[u8]) {
        let len = s.len().min(self.prefix.len() - 1);
        self.prefix = [0; 64];
        self.prefix[..len].copy_from_slice(&s[..len]);
    }
}

struct Plugin {
    host: &'static oshot_host_t, // oshot's table outlives the plugin
    ctx: *mut oshot_ctx_t,

    saved: Prefs,   // what's in config.toml
    editing: Prefs, // what the Preferences window shows, until saved

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

    fn set_bool(&self, key: &CStr, val: bool) {
        unsafe { host_fn!(self.host, set_bool)(self.ctx, OSHOT_STORE_CONFIG, key.as_ptr(), val) }
    }

    fn get_string(&self, key: &CStr, fallback: &[u8]) -> HostString<'static> {
        let s = unsafe {
            host_fn!(self.host, get_string)(self.ctx, OSHOT_STORE_CONFIG, key.as_ptr(), oshot_str_t::borrow(fallback))
        };
        HostString { host: self.host, s }
    }

    fn set_string(&self, key: &CStr, val: &[u8]) {
        unsafe { host_fn!(self.host, set_string)(self.ctx, OSHOT_STORE_CONFIG, key.as_ptr(), oshot_str_t::borrow(val)) }
    }

    fn get_text(&self, id: &CStr) -> Option<HostString<'static>> {
        let mut s = oshot_str_t { p: std::ptr::null(), len: 0 };
        unsafe { host_fn!(self.host, get_text)(self.ctx, id.as_ptr(), &mut s) }
            .then_some(HostString { host: self.host, s })
    }

    fn set_text(&self, id: &CStr, val: &[u8]) {
        unsafe { host_fn!(self.host, set_text)(self.ctx, id.as_ptr(), oshot_str_t::borrow(val)) };
    }

    /// Width and height of the selection. get_capture() copies the whole
    /// image: only on demand, never every frame.
    fn capture_size(&self) -> (i32, i32) {
        unsafe {
            let mut cap = host_fn!(self.host, get_capture)(self.ctx);
            let size = (cap.w, cap.h);
            host_fn!(self.host, capture_free)(&mut cap);
            size
        }
    }
}

// What the plugin does
impl Plugin {
    fn new(host: &'static oshot_host_t, ctx: *mut oshot_ctx_t) -> Self {
        let mut plugin = Plugin {
            host,
            ctx,
            saved: Prefs { auto_uppercase: false, prefix: [0; 64] },
            editing: Prefs { auto_uppercase: false, prefix: [0; 64] },
            capture_size: None,
        };

        plugin.saved.auto_uppercase = plugin.get_bool(c"auto-uppercase", false);
        let prefix = plugin.get_string(c"prefix", b"");
        plugin.saved.set_prefix(prefix.as_bytes());
        plugin.editing = plugin.saved.clone();

        plugin.log(OSHOT_LOG_INFO, "uppercase plugin (Rust) loaded");
        plugin
    }

    /// Replaces oshot's OCR output with prefix + UPPERCASE(text)
    fn uppercase_text(&self, text: &[u8]) {
        let mut out = String::from_utf8_lossy(self.saved.prefix()).into_owned();
        out.push_str(&String::from_utf8_lossy(text).to_uppercase());
        self.set_text(OSHOT_TEXT_OCR, out.as_bytes());
    }

    fn render(&mut self) {
        if unsafe { igButton(c"Uppercase the OCR output".as_ptr(), ImVec2_c { x: 0.0, y: 0.0 }) } {
            if let Some(text) = self.get_text(OSHOT_TEXT_OCR) {
                self.uppercase_text(text.as_bytes());
            }
            self.capture_size = Some(self.capture_size());
        }

        if let Some((w, h)) = self.capture_size {
            // Formatted in Rust; "%s" keeps ImGui from reading the text as a format
            let line = CString::new(format!("Selection: {w}x{h}")).unwrap();
            unsafe { igTextDisabled(c"%s".as_ptr(), line.as_ptr()) }
        }
    }

    fn on_ocr_done(&self, result: &oshot_ocr_result_t) {
        if self.saved.auto_uppercase {
            // SAFETY: the text is valid during on_ocr_done()
            self.uppercase_text(unsafe { result.text.as_bytes() });
        }
    }

    fn render_preferences(&mut self) -> bool {
        let prefix = &mut self.editing.prefix;
        unsafe {
            igCheckbox(c"Uppercase as soon as text is extracted".as_ptr(), &mut self.editing.auto_uppercase);
            igInputText(
                c"Prefix".as_ptr(),
                prefix.as_mut_ptr().cast::<c_char>(),
                prefix.len(),
                ImGuiInputTextFlags_None as ImGuiInputTextFlags,
                None,
                std::ptr::null_mut(),
            );
        }
        self.editing != self.saved
    }

    fn on_save_preferences(&mut self) {
        self.saved = self.editing.clone();
        self.set_bool(c"auto-uppercase", self.saved.auto_uppercase);
        self.set_string(c"prefix", self.saved.prefix());
    }

    fn on_discard_preferences(&mut self) {
        self.editing = self.saved.clone();
    }
}

// The C callbacks. A panic can't unwind into oshot: Rust aborts instead, so
// oshot exits right there rather than continuing in a broken state.

unsafe extern "C" fn plugin_init(host: *const oshot_host_t, ctx: *mut oshot_ctx_t, state: *mut State) -> bool {
    // SAFETY: oshot's host table lives as long as the process
    let plugin = Box::new(Plugin::new(unsafe { &*host }, ctx));
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

unsafe extern "C" fn plugin_render_preferences(state: State) -> bool {
    unsafe { plugin(state) }.render_preferences()
}

unsafe extern "C" fn plugin_on_save_preferences(state: State) {
    unsafe { plugin(state) }.on_save_preferences();
}

unsafe extern "C" fn plugin_on_discard_preferences(state: State) {
    unsafe { plugin(state) }.on_discard_preferences();
}

static DESCRIPTOR: oshot_plugin_t = oshot_plugin_t {
    struct_size: size_of::<oshot_plugin_t>() as u32,
    abi_version: OSHOT_API_VERSION,
    imgui_version: OSHOT_IMGUI_VERSION,
    id: c"io.github.toni500github.uppercase-rust".as_ptr(),
    name: c"Uppercase (Rust)".as_ptr(),
    init: Some(plugin_init),
    destroy: Some(plugin_destroy),
    render: Some(plugin_render),
    on_ocr_done: Some(plugin_on_ocr_done),
    render_preferences: Some(plugin_render_preferences),
    on_save_preferences: Some(plugin_on_save_preferences),
    on_discard_preferences: Some(plugin_on_discard_preferences),
};

/// The one symbol the library exports
#[unsafe(no_mangle)]
pub extern "C" fn oshot_plugin_entry() -> *const oshot_plugin_t {
    &DESCRIPTOR
}
