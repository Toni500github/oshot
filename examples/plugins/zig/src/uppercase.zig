//! Example oshot plugin in Zig: uppercases the OCR output. It does what the C
//! example (../c/uppercase.c) does, with oshot_plugin.h translated by build.zig
//! into the "c" module.
//!
//! - render():      a button that uppercases the text, and the selection's size
//! - on_ocr_done(): optionally uppercases new text right away
//! - settings:      that option and a prefix, which oshot draws in Preferences > Plugins
//!                  and saves in the plugin's config.toml
//!
//! It draws only with the host's ui_* functions (imgui_version = 0).
//!
//! Build it with `zig build --release=safe` (see build.zig), or install the
//! folder with `oshotpm install <this folder>`. See docs/plugins.md. Public
//! domain (Unlicense).

const std = @import("std");
const c = @import("c");

const allocator = std.heap.c_allocator;

fn borrow(s: []const u8) c.oshot_str_t {
    return .{ .p = s.ptr, .len = s.len };
}

/// A string oshot returned; free it with host.str_free
fn hostSlice(s: c.oshot_str_t) []const u8 {
    return if (s.p) |p| p[0..s.len] else "";
}

/// OSHOT_HOST_HAS(), which translate-c can't translate
fn hostHas(host: *const c.oshot_host_t, comptime field: []const u8) bool {
    return @offsetOf(c.oshot_host_t, field) + @sizeOf(@FieldType(c.oshot_host_t, field)) <= host.struct_size;
}

/// oshot draws these in Preferences > Plugins and saves them in config.toml.
/// Both fallbacks are the zeroed value (false, an empty string); translate-c
/// names the value's anonymous union `unnamed_0` for other ones.
const settings = [_]c.oshot_setting_t{
    .{
        .kind = c.OSHOT_SETTING_BOOL,
        .key = "auto-uppercase",
        .label = "Uppercase as soon as text is extracted",
        .fallback = .{ .kind = c.OSHOT_VAL_BOOL },
    },
    .{
        .kind = c.OSHOT_SETTING_STRING,
        .key = "prefix",
        .label = "Prefix",
        .help = "Put before the uppercased text",
        .fallback = .{ .kind = c.OSHOT_VAL_STRING },
    },
};

const Plugin = struct {
    host: *const c.oshot_host_t,
    ctx: ?*c.oshot_ctx_t,

    // The settings, as last saved
    auto_uppercase: bool = false, // uppercase as soon as OCR is done
    prefix: c.oshot_str_t = .{ .p = null, .len = 0 }, // put before the uppercased text; a host string

    cap_w: i32 = 0, // size of the selection when "Uppercase" was last pressed
    cap_h: i32 = 0,

    fn init(host: *const c.oshot_host_t, ctx: ?*c.oshot_ctx_t) Plugin {
        var self: Plugin = .{ .host = host, .ctx = ctx };
        self.readSettings();
        self.log(c.OSHOT_LOG_INFO, "uppercase plugin (Zig) loaded");
        return self;
    }

    fn deinit(self: *Plugin) void {
        self.host.str_free.?(&self.prefix);
    }

    fn log(self: *const Plugin, level: c.oshot_log_level_t, msg: []const u8) void {
        self.host.log.?(self.ctx, level, borrow(msg));
    }

    /// Reads the settings with the same keys and fallbacks as `settings`
    fn readSettings(self: *Plugin) void {
        self.auto_uppercase = self.host.get_bool.?(self.ctx, c.OSHOT_STORE_CONFIG, "auto-uppercase", false);
        self.host.str_free.?(&self.prefix);
        self.prefix = self.host.get_string.?(self.ctx, c.OSHOT_STORE_CONFIG, "prefix", borrow(""));
    }

    /// Replaces oshot's OCR output with prefix + UPPERCASE(text)
    fn uppercaseText(self: *const Plugin, text: []const u8) void {
        const prefix = hostSlice(self.prefix);
        const out = allocator.alloc(u8, prefix.len + text.len) catch {
            self.log(c.OSHOT_LOG_ERROR, "out of memory");
            return;
        };
        defer allocator.free(out);

        @memcpy(out[0..prefix.len], prefix);
        _ = std.ascii.upperString(out[prefix.len..], text);
        _ = self.host.set_text.?(self.ctx, c.OSHOT_TEXT_OCR, borrow(out));
    }

    fn render(self: *Plugin) void {
        if (self.host.ui_button.?(self.ctx, borrow("Uppercase the OCR output"))) {
            var text: c.oshot_str_t = undefined;
            if (self.host.get_text.?(self.ctx, c.OSHOT_TEXT_OCR, &text)) {
                defer self.host.str_free.?(&text);
                self.uppercaseText(hostSlice(text));
            }

            // get_capture() copies the whole image, so only on demand, never every frame
            var cap = self.host.get_capture.?(self.ctx);
            defer self.host.capture_free.?(&cap);
            self.cap_w = cap.w;
            self.cap_h = cap.h;
        }

        if (self.cap_w > 0) {
            var buf: [64]u8 = undefined;
            const line = std.fmt.bufPrint(&buf, "Selection: {d}x{d}", .{ self.cap_w, self.cap_h }) catch unreachable;
            self.host.ui_text.?(self.ctx, borrow(line), c.OSHOT_UI_DISABLED);
        }
    }

    fn onOcrDone(self: *Plugin, result: *const c.oshot_ocr_result_t) void {
        if (self.auto_uppercase and result.text.len > 0) // empty when nothing was read
            self.uppercaseText(hostSlice(result.text));
    }
};

// The C callbacks. With --release=safe a failed safety check panics, which
// ends oshot right there instead of letting it continue in a broken state.

fn fromState(state: ?*anyopaque) *Plugin {
    return @ptrCast(@alignCast(state.?));
}

fn pluginInit(host: [*c]const c.oshot_host_t, ctx: ?*c.oshot_ctx_t, state: [*c]?*anyopaque) callconv(.c) bool {
    // The ui_* functions came after the first plugin API: check the last one
    if (!hostHas(host, "ui_end_section")) {
        host.*.log.?(ctx, c.OSHOT_LOG_ERROR, borrow("this oshot is too old for this plugin: update it"));
        return false;
    }

    const plugin = allocator.create(Plugin) catch return false;
    plugin.* = .init(host, ctx);
    state.* = plugin;
    return true;
}

fn pluginDestroy(state: ?*anyopaque) callconv(.c) void {
    const plugin = fromState(state);
    plugin.deinit();
    allocator.destroy(plugin);
}

fn pluginRender(state: ?*anyopaque) callconv(.c) void {
    fromState(state).render();
}

fn pluginOnOcrDone(state: ?*anyopaque, result: [*c]const c.oshot_ocr_result_t) callconv(.c) void {
    fromState(state).onOcrDone(result);
}

fn pluginOnSettingsSaved(state: ?*anyopaque) callconv(.c) void {
    fromState(state).readSettings();
}

const descriptor: c.oshot_plugin_t = .{
    .struct_size = @sizeOf(c.oshot_plugin_t),
    .abi_version = c.OSHOT_API_VERSION,
    .imgui_version = 0, // no cimgui
    .id = "io.github.toni500github.uppercase-zig",
    .name = "Uppercase (Zig)",
    .init = pluginInit,
    .destroy = pluginDestroy,
    .render = pluginRender,
    .on_ocr_done = pluginOnOcrDone,
    .settings = &settings,
    .n_settings = settings.len,
    .on_settings_saved = pluginOnSettingsSaved,
};

/// The one symbol the library exports
export fn oshot_plugin_entry() *const c.oshot_plugin_t {
    return &descriptor;
}
