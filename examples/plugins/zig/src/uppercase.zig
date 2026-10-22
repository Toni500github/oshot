//! Example oshot plugin in Zig: uppercases the OCR output. It does what the C
//! example (../c/uppercase.c) does, with the SDK headers translated by
//! build.zig into the "c" module.
//!
//! - render():             a button that uppercases the text, and the selection's size
//! - on_ocr_done():        optionally uppercases new text right away
//! - render_preferences(): a setting for that, plus a prefix, saved in the plugin's config.toml
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

const Prefs = struct {
    auto_uppercase: bool = false, // uppercase as soon as OCR is done
    prefix: [64]u8 = @splat(0), // put before the uppercased text; a NUL-terminated buffer for igInputText

    fn prefixSlice(self: *const Prefs) []const u8 {
        return std.mem.sliceTo(&self.prefix, 0);
    }

    fn setPrefix(self: *Prefs, s: []const u8) void {
        const len = @min(s.len, self.prefix.len - 1);
        self.prefix = @splat(0);
        @memcpy(self.prefix[0..len], s[0..len]);
    }

    fn eql(a: *const Prefs, b: *const Prefs) bool {
        return a.auto_uppercase == b.auto_uppercase and std.mem.eql(u8, a.prefixSlice(), b.prefixSlice());
    }
};

const Plugin = struct {
    host: *const c.oshot_host_t,
    ctx: ?*c.oshot_ctx_t,

    saved: Prefs = .{}, // what's in config.toml
    editing: Prefs = .{}, // what the Preferences window shows, until saved

    cap_w: i32 = 0, // size of the selection when "Uppercase" was last pressed
    cap_h: i32 = 0,

    fn init(host: *const c.oshot_host_t, ctx: ?*c.oshot_ctx_t) Plugin {
        var self: Plugin = .{ .host = host, .ctx = ctx };

        self.saved.auto_uppercase = host.get_bool.?(ctx, c.OSHOT_STORE_CONFIG, "auto-uppercase", false);
        var prefix = host.get_string.?(ctx, c.OSHOT_STORE_CONFIG, "prefix", borrow(""));
        defer host.str_free.?(&prefix);
        self.saved.setPrefix(hostSlice(prefix));
        self.editing = self.saved;

        self.log(c.OSHOT_LOG_INFO, "uppercase plugin (Zig) loaded");
        return self;
    }

    fn log(self: *const Plugin, level: c.oshot_log_level_t, msg: []const u8) void {
        self.host.log.?(self.ctx, level, borrow(msg));
    }

    /// Replaces oshot's OCR output with prefix + UPPERCASE(text)
    fn uppercaseText(self: *const Plugin, text: []const u8) void {
        const prefix = self.saved.prefixSlice();
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
        if (c.igButton("Uppercase the OCR output", .{ .x = 0, .y = 0 })) {
            var text: c.oshot_str_t = undefined;
            if (self.host.get_text.?(self.ctx, c.OSHOT_TEXT_OCR, &text)) {
                defer self.host.str_free.?(&text);
                self.uppercaseText(hostSlice(text));
            }

            // get_capture() copies the whole image: only on demand, never every frame
            var cap = self.host.get_capture.?(self.ctx);
            defer self.host.capture_free.?(&cap);
            self.cap_w = cap.w;
            self.cap_h = cap.h;
        }

        if (self.cap_w > 0)
            c.igTextDisabled("Selection: %dx%d", @as(c_int, self.cap_w), @as(c_int, self.cap_h));
    }

    fn onOcrDone(self: *Plugin, result: *const c.oshot_ocr_result_t) void {
        if (self.saved.auto_uppercase)
            self.uppercaseText(hostSlice(result.text));
    }

    fn renderPreferences(self: *Plugin) bool {
        _ = c.igCheckbox("Uppercase as soon as text is extracted", &self.editing.auto_uppercase);
        _ = c.igInputText("Prefix", &self.editing.prefix, self.editing.prefix.len, c.ImGuiInputTextFlags_None, null, null);
        return !self.editing.eql(&self.saved);
    }

    fn onSavePreferences(self: *Plugin) void {
        self.saved = self.editing;
        self.host.set_bool.?(self.ctx, c.OSHOT_STORE_CONFIG, "auto-uppercase", self.saved.auto_uppercase);
        self.host.set_string.?(self.ctx, c.OSHOT_STORE_CONFIG, "prefix", borrow(self.saved.prefixSlice()));
    }

    fn onDiscardPreferences(self: *Plugin) void {
        self.editing = self.saved;
    }
};

// The C callbacks. With --release=safe a failed safety check panics, which
// ends oshot right there instead of letting it continue in a broken state.

fn fromState(state: ?*anyopaque) *Plugin {
    return @ptrCast(@alignCast(state.?));
}

fn pluginInit(host: [*c]const c.oshot_host_t, ctx: ?*c.oshot_ctx_t, state: [*c]?*anyopaque) callconv(.c) bool {
    const plugin = allocator.create(Plugin) catch return false;
    plugin.* = .init(host, ctx);
    state.* = plugin;
    return true;
}

fn pluginDestroy(state: ?*anyopaque) callconv(.c) void {
    allocator.destroy(fromState(state));
}

fn pluginRender(state: ?*anyopaque) callconv(.c) void {
    fromState(state).render();
}

fn pluginOnOcrDone(state: ?*anyopaque, result: [*c]const c.oshot_ocr_result_t) callconv(.c) void {
    fromState(state).onOcrDone(result);
}

fn pluginRenderPreferences(state: ?*anyopaque) callconv(.c) bool {
    return fromState(state).renderPreferences();
}

fn pluginOnSavePreferences(state: ?*anyopaque) callconv(.c) void {
    fromState(state).onSavePreferences();
}

fn pluginOnDiscardPreferences(state: ?*anyopaque) callconv(.c) void {
    fromState(state).onDiscardPreferences();
}

const descriptor: c.oshot_plugin_t = .{
    .struct_size = @sizeOf(c.oshot_plugin_t),
    .abi_version = c.OSHOT_API_VERSION,
    .imgui_version = c.OSHOT_IMGUI_VERSION,
    .id = "io.github.toni500github.uppercase-zig",
    .name = "Uppercase (Zig)",
    .init = pluginInit,
    .destroy = pluginDestroy,
    .render = pluginRender,
    .on_ocr_done = pluginOnOcrDone,
    .render_preferences = pluginRenderPreferences,
    .on_save_preferences = pluginOnSavePreferences,
    .on_discard_preferences = pluginOnDiscardPreferences,
};

/// The one symbol the library exports
export fn oshot_plugin_entry() *const c.oshot_plugin_t {
    return &descriptor;
}
