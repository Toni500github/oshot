//! Bindings to oshot's plugin API (oshot/oshot_plugin.h in oshot's SDK),
//! written by hand. oshot only ever appends to its structs, so these keep
//! working with newer versions of oshot; copy the fields a newer header adds
//! to use them. A plugin that also calls cimgui is better off generating its
//! bindings with bindgen.

#![allow(non_camel_case_types, dead_code)]

use std::ffi::{CStr, c_char, c_int, c_void};

pub const OSHOT_API_VERSION: u32 = 1;

pub const OSHOT_TEXT_OCR: &CStr = c"ocr_output";
pub const OSHOT_TEXT_BARCODE: &CStr = c"barcode_output";

pub const OSHOT_UI_DISABLED: u32 = 1 << 0;
pub const OSHOT_UI_WRAPPED: u32 = 1 << 1;
pub const OSHOT_UI_MULTILINE: u32 = 1 << 2;
pub const OSHOT_UI_PASSWORD: u32 = 1 << 3;

pub const OSHOT_PROCESS_OUTPUT_MAX: usize = 4 * 1024 * 1024;

/// Opaque: identifies the plugin in every host call
#[repr(C)]
pub struct oshot_ctx_t {
    _private: [u8; 0],
}

// The C enums, which are ints
pub type oshot_log_level_t = c_int;
pub const OSHOT_LOG_DEBUG: oshot_log_level_t = 0;
pub const OSHOT_LOG_INFO: oshot_log_level_t = 1;
pub const OSHOT_LOG_WARN: oshot_log_level_t = 2;
pub const OSHOT_LOG_ERROR: oshot_log_level_t = 3;

// Not a Rust enum: oshot may return codes added after these bindings
pub type oshot_err_t = c_int;
pub const OSHOT_OK: oshot_err_t = 0;
pub const OSHOT_ERR_INVALID: oshot_err_t = 1;
pub const OSHOT_ERR_THREAD: oshot_err_t = 2;
pub const OSHOT_ERR_UNSUPPORTED: oshot_err_t = 3;
pub const OSHOT_ERR_NOT_FOUND: oshot_err_t = 4;
pub const OSHOT_ERR_FAILED: oshot_err_t = 5;

pub type oshot_value_kind_t = c_int;
pub const OSHOT_VAL_STRING: oshot_value_kind_t = 0;
pub const OSHOT_VAL_INT64: oshot_value_kind_t = 1;
pub const OSHOT_VAL_BOOL: oshot_value_kind_t = 2;
pub const OSHOT_VAL_DOUBLE: oshot_value_kind_t = 3;

pub type oshot_store_t = c_int;
pub const OSHOT_STORE_CONFIG: oshot_store_t = 0;
pub const OSHOT_STORE_CACHE: oshot_store_t = 1;

pub type oshot_setting_kind_t = c_int;
pub const OSHOT_SETTING_BOOL: oshot_setting_kind_t = 0;
pub const OSHOT_SETTING_STRING: oshot_setting_kind_t = 1;
pub const OSHOT_SETTING_INT: oshot_setting_kind_t = 2;
pub const OSHOT_SETTING_FLOAT: oshot_setting_kind_t = 3;
pub const OSHOT_SETTING_CHOICE: oshot_setting_kind_t = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct oshot_str_t {
    pub p: *const c_char,
    pub len: usize,
}

impl oshot_str_t {
    pub const EMPTY: Self = Self { p: std::ptr::null(), len: 0 };

    /// Borrows `s` without copying, like oshot_str_borrow()
    pub fn borrow(s: &[u8]) -> Self {
        Self { p: s.as_ptr().cast(), len: s.len() }
    }

    /// # Safety
    /// `p` must point to `len` readable bytes, or be null
    pub unsafe fn as_bytes(&self) -> &[u8] {
        if self.p.is_null() { &[] } else { unsafe { std::slice::from_raw_parts(self.p.cast(), self.len) } }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union oshot_value_u {
    pub s: oshot_str_t,
    pub i: i64,
    pub b: bool,
    pub d: f64,
}

/// The C struct's anonymous union is `v` here
#[repr(C)]
#[derive(Clone, Copy)]
pub struct oshot_value_t {
    pub kind: oshot_value_kind_t,
    pub v: oshot_value_u,
}

#[repr(C)]
pub struct oshot_capture_t {
    pub w: i32,
    pub h: i32,
    pub rgba: *mut u8,
}

#[repr(C)]
pub struct oshot_ocr_word_t {
    pub text: oshot_str_t,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub confidence: i32,
    pub line: i32,
    pub block: i32,
}

#[repr(C)]
pub struct oshot_barcode_t {
    pub type_: oshot_str_t, // `type` in C
    pub data: oshot_str_t,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

#[repr(C)]
pub struct oshot_ocr_result_t {
    pub text: oshot_str_t,
    pub confidence: i32,
    pub psm: i32,
    // Only there if ocr_has_words!(): an older oshot's struct ends before them
    pub words: *const oshot_ocr_word_t,
    pub n_words: usize,
}

#[repr(C)]
pub struct oshot_process_result_t {
    pub id: i64,
    pub exit_code: i32,
    pub killed: bool,
    pub truncated: bool,
    pub stdout_data: oshot_str_t,
    pub stderr_data: oshot_str_t,
}

#[repr(C)]
pub struct oshot_setting_t {
    pub kind: oshot_setting_kind_t,
    pub key: *const c_char,
    pub label: *const c_char,
    pub help: *const c_char,
    pub fallback: oshot_value_t,
    pub min: f64,
    pub max: f64,
    pub choices: *const *const c_char,
    pub n_choices: usize,
    pub flags: u32,
}

impl oshot_setting_t {
    /// A setting with only its kind, key, label and fallback; set the rest with `..`
    pub const fn new(
        kind: oshot_setting_kind_t,
        key: &'static CStr,
        label: &'static CStr,
        fallback: oshot_value_t,
    ) -> Self {
        Self {
            kind,
            key: key.as_ptr(),
            label: label.as_ptr(),
            help: std::ptr::null(),
            fallback,
            min: 0.0,
            max: 0.0,
            choices: std::ptr::null(),
            n_choices: 0,
            flags: 0,
        }
    }
}

type Ctx = *mut oshot_ctx_t;

#[repr(C)]
pub struct oshot_host_t {
    pub struct_size: u32,
    pub abi_version: u32,
    pub oshot_version: *const c_char,

    pub str_new: Option<unsafe extern "C" fn(str: *const c_char, len: usize) -> oshot_str_t>,
    pub str_free: Option<unsafe extern "C" fn(str: *mut oshot_str_t)>,
    pub value_array_free: Option<unsafe extern "C" fn(arr: *mut oshot_value_t, n: usize)>,
    pub log: Option<unsafe extern "C" fn(ctx: Ctx, level: oshot_log_level_t, msg: oshot_str_t)>,
    pub get_data_dir: Option<unsafe extern "C" fn(ctx: Ctx) -> oshot_str_t>,
    pub run_on_ui_thread:
        Option<unsafe extern "C" fn(ctx: Ctx, f: Option<unsafe extern "C" fn(user: *mut c_void)>, user: *mut c_void)>,

    pub get_string: Option<
        unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, fallback: oshot_str_t) -> oshot_str_t,
    >,
    pub get_bool:
        Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, fallback: bool) -> bool>,
    pub get_int64:
        Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, fallback: i64) -> i64>,
    pub get_double:
        Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, fallback: f64) -> f64>,
    pub get_array: Option<
        unsafe extern "C" fn(
            ctx: Ctx,
            store: oshot_store_t,
            key: *const c_char,
            out: *mut oshot_value_t,
            max: usize,
        ) -> usize,
    >,
    pub get_value: Option<
        unsafe extern "C" fn(
            ctx: Ctx,
            store: oshot_store_t,
            key: *const c_char,
            fallback: oshot_value_t,
        ) -> oshot_value_t,
    >,

    pub set_string: Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, val: oshot_str_t)>,
    pub set_bool: Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, val: bool)>,
    pub set_int64: Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, val: i64)>,
    pub set_double: Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, val: f64)>,
    pub set_value:
        Option<unsafe extern "C" fn(ctx: Ctx, store: oshot_store_t, key: *const c_char, val: *const oshot_value_t)>,

    pub get_text: Option<unsafe extern "C" fn(ctx: Ctx, id: *const c_char, out: *mut oshot_str_t) -> bool>,
    pub set_text: Option<unsafe extern "C" fn(ctx: Ctx, id: *const c_char, value: oshot_str_t) -> bool>,

    pub get_capture: Option<unsafe extern "C" fn(ctx: Ctx) -> oshot_capture_t>,
    pub capture_free: Option<unsafe extern "C" fn(cap: *mut oshot_capture_t)>,

    // Added after the first version: check host_has!() first
    pub ui_text: Option<unsafe extern "C" fn(ctx: Ctx, text: oshot_str_t, flags: u32)>,
    pub ui_button: Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t) -> bool>,
    pub ui_checkbox: Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t, value: *mut bool) -> bool>,
    pub ui_input_text:
        Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t, value: *mut oshot_str_t, flags: u32) -> bool>,
    pub ui_combo: Option<
        unsafe extern "C" fn(
            ctx: Ctx,
            label: oshot_str_t,
            current: *mut i32,
            items: *const oshot_str_t,
            n: usize,
        ) -> bool,
    >,
    pub ui_slider_int:
        Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t, value: *mut i64, min: i64, max: i64) -> bool>,
    pub ui_slider_float:
        Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t, value: *mut f64, min: f64, max: f64) -> bool>,
    pub ui_color: Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t, rgba: *mut f32) -> bool>,
    pub ui_progress: Option<unsafe extern "C" fn(ctx: Ctx, fraction: f32, overlay: oshot_str_t)>,
    pub ui_separator: Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t)>,
    pub ui_same_line: Option<unsafe extern "C" fn(ctx: Ctx)>,
    pub ui_tooltip: Option<unsafe extern "C" fn(ctx: Ctx, text: oshot_str_t)>,
    pub ui_begin_section: Option<unsafe extern "C" fn(ctx: Ctx, label: oshot_str_t) -> bool>,
    pub ui_end_section: Option<unsafe extern "C" fn(ctx: Ctx)>,

    // Host services: check host_has!(host, kill_process) first
    pub mem_alloc: Option<unsafe extern "C" fn(size: usize) -> *mut c_void>,
    pub mem_free: Option<unsafe extern "C" fn(ptr: *mut c_void)>,
    pub copy_text: Option<unsafe extern "C" fn(ctx: Ctx, text: oshot_str_t, err: *mut oshot_str_t) -> oshot_err_t>,
    pub copy_image:
        Option<unsafe extern "C" fn(ctx: Ctx, image: *const oshot_capture_t, err: *mut oshot_str_t) -> oshot_err_t>,
    pub notify: Option<
        unsafe extern "C" fn(ctx: Ctx, title: oshot_str_t, body: oshot_str_t, err: *mut oshot_str_t) -> oshot_err_t,
    >,
    pub open_url: Option<unsafe extern "C" fn(ctx: Ctx, url: oshot_str_t, err: *mut oshot_str_t) -> oshot_err_t>,
    pub run_process: Option<
        unsafe extern "C" fn(
            ctx: Ctx,
            argv: *const oshot_str_t,
            argc: usize,
            cwd: oshot_str_t,
            on_exit: Option<unsafe extern "C" fn(user: *mut c_void, result: *const oshot_process_result_t)>,
            user: *mut c_void,
            id: *mut i64,
            err: *mut oshot_str_t,
        ) -> oshot_err_t,
    >,
    pub kill_process: Option<unsafe extern "C" fn(ctx: Ctx, id: i64, err: *mut oshot_str_t) -> oshot_err_t>,
}

type State = *mut c_void;

#[repr(C)]
pub struct oshot_plugin_t {
    pub struct_size: u32,
    pub abi_version: u32,
    pub imgui_version: u32,
    pub id: *const c_char,
    pub name: *const c_char,

    pub init: Option<unsafe extern "C" fn(host: *const oshot_host_t, ctx: Ctx, state: *mut State) -> bool>,
    pub destroy: Option<unsafe extern "C" fn(state: State)>,
    pub render: Option<unsafe extern "C" fn(state: State)>,
    pub on_ocr_done: Option<unsafe extern "C" fn(state: State, result: *const oshot_ocr_result_t)>,
    pub render_preferences: Option<unsafe extern "C" fn(state: State) -> bool>,
    pub on_save_preferences: Option<unsafe extern "C" fn(state: State)>,
    pub on_discard_preferences: Option<unsafe extern "C" fn(state: State)>,

    // Added after the first version
    pub settings: *const oshot_setting_t,
    pub n_settings: usize,
    pub on_settings_saved: Option<unsafe extern "C" fn(state: State)>,
    pub on_barcodes_done: Option<unsafe extern "C" fn(state: State, codes: *const oshot_barcode_t, n: usize)>,
    pub on_capture_start: Option<unsafe extern "C" fn(state: State)>,
    pub on_capture_end: Option<unsafe extern "C" fn(state: State)>,
    pub on_selection_changed: Option<unsafe extern "C" fn(state: State, x: f32, y: f32, w: f32, h: f32)>,
}

// The descriptor and the settings are statics whose pointers are to string
// literals and other statics, which never change, so sharing them between
// threads is fine
unsafe impl Sync for oshot_plugin_t {}
unsafe impl Sync for oshot_setting_t {}

/// OSHOT_HOST_HAS(): whether the running oshot's host table has `field`.
/// Every field after `oshot_version` is a function pointer.
macro_rules! host_has {
    ($host:expr, $field:ident) => {
        std::mem::offset_of!(oshot_host_t, $field) + size_of::<*const ()>() <= $host.struct_size as usize
    };
}
pub(crate) use host_has;

/// OSHOT_OCR_HAS_WORDS(): whether oshot_ocr_result_t has `words` and `n_words`,
/// which came with mem_alloc
#[allow(unused_macros)]
macro_rules! ocr_has_words {
    ($host:expr) => {
        $crate::ffi::host_has!($host, mem_alloc)
    };
}
#[allow(unused_imports)]
pub(crate) use ocr_has_words;

/// A function from oshot's host table. oshot fills in every field of the API
/// version it was built with; a field added in a later version needs a
/// `host_has!` check before it's used.
macro_rules! host_fn {
    ($host:expr, $f:ident) => {
        $host.$f.expect(concat!("oshot's host table has no ", stringify!($f)))
    };
}
pub(crate) use host_fn;
