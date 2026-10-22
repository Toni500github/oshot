//! Builds the Zig example plugin against oshot's SDK, into zig-out/plugin:
//!     zig build --release=safe -Dsdk=<sdk dir>
//! oshotpm sets the OSHOT_SDK environment variable when it builds plugins,
//! which is used when -Dsdk isn't given. Written for Zig 0.17.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const sdk_path = b.option([]const u8, "sdk", "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h") orelse
        b.graph.environ_map.get("OSHOT_SDK") orelse
        std.debug.panic("oshot's plugin SDK wasn't found: pass -Dsdk=<folder with include/oshot/oshot_plugin.h>", .{});
    const sdk: std.Build.LazyPath = .{ .cwd_relative = sdk_path };

    // The SDK headers become the "c" module, so the plugin always uses the
    // structs and version numbers of the oshot it's built for
    const headers = b.addTranslateC(.{
        .root_source_file = b.path("src/oshot.h"),
        .target = target,
        .optimize = optimize,
    });
    headers.addIncludePath(sdk.path(b, "include"));

    const mod = b.createModule(.{
        .root_source_file = b.path("src/uppercase.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    mod.addImport("c", headers.createModule());

    const lib = b.addLibrary(.{
        .name = "uppercase_zig",
        .linkage = .dynamic,
        .root_module = mod,
    });

    // cimgui comes from oshot: Linux and the BSDs resolve it when oshot loads
    // the plugin, macOS needs to be told to, Windows links its import library
    if (target.result.os.tag == .windows) {
        mod.addLibraryPath(sdk.path(b, "lib"));
        mod.linkSystemLibrary("cimgui", .{});
    } else {
        lib.linker_allow_shlib_undefined = true;
    }

    // Only the library, without the Windows import library or debug files,
    // as oshotpm wants a folder with exactly one library
    const install = b.addInstallArtifact(lib, .{
        .dest_dir = .{ .override = .{ .custom = "plugin" } },
        .implib_dir = .disabled,
        .pdb_dir = .disabled,
        .dylib_symlinks = false,
    });
    b.getInstallStep().dependOn(&install.step);
}
