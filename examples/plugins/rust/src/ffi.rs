//! The bindings build.rs generates from oshot's SDK (oshot/oshot_plugin.h and
//! all of cimgui), plus a few helpers for their types.

#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code, clippy::all)]

include!(concat!(env!("OUT_DIR"), "/oshot.rs"));

impl oshot_str_t {
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

// The descriptor is a static: its pointers are to string literals, which
// never change, so sharing it between threads is fine
unsafe impl Sync for oshot_plugin_t {}

/// A function from oshot's host table. oshot fills in every field of the API
/// version it was built with; a field added in a later version needs a
/// `host.struct_size` check before it's used.
macro_rules! host_fn {
    ($host:expr, $f:ident) => {
        $host.$f.expect(concat!("oshot's host table has no ", stringify!($f)))
    };
}
pub(crate) use host_fn;
