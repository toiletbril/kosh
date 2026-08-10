const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});

    const optimize = b.standardOptimizeOption(.{});
    const is_release = optimize != .Debug;
    const mode = if (is_release) "rel" else "dbg";

    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });

    const exe = b.addExecutable(.{
        .name = if (is_release) "kosh" else "kosh-dbg",
        .root_module = module,
    });

    var flags: std.ArrayList([]const u8) = .empty;
    defer flags.deinit(b.allocator);

    const common_flags = [_][]const u8{
        "-std=c++23",
        "-Wall",
        "-Wextra",
        "-Wdouble-promotion",
        "-Wno-format",
        "-Wno-unused-command-line-argument",
        "-Wno-deprecated",
        "-Wno-unknown-warning-option",
        "-Wno-error=maybe-uninitialized",
        "-fvisibility=hidden",
        "-fno-rtti",
        "-fno-semantic-interposition",
        "-Wno-unknown-pragmas",
        "-fno-strict-aliasing",
        "-Wno-date-time",
    };
    flags.appendSlice(b.allocator, &common_flags) catch @panic("out of memory");

    const commit_hash = gitCommitHash(b);
    const os_info = b.fmt("zig {f} {s}", .{ @import("builtin").zig_version, systemInfo(b) });

    const defines = [_][]const u8{
        "-DKOSH_COMPILER_COMMAND=\"zig c++\"",
        "-DKOSH_LIBC=\"zig libc++\"",
        b.fmt("-DKOSH_OS_INFO=\"{s}\"", .{os_info}),
        b.fmt("-DKOSH_COMMIT_HASH=\"{s}\"", .{commit_hash}),
        b.fmt("-DKOSH_BUILD_MODE=\"{s}\"", .{mode}),
        "-DKOSH_ENVCXXFLAGS=\"\"",
    };
    flags.appendSlice(b.allocator, &defines) catch @panic("out of memory");

    const sources = collectSources(b);
    module.addCSourceFiles(.{
        .root = b.path("src"),
        .files = sources,
        .flags = flags.items,
    });

    b.installArtifact(exe);

    const run_step = b.step("run", "Build and run kosh");
    const run_cmd = b.addRunArtifact(exe);
    if (b.args) |args| run_cmd.addArgs(args);
    run_step.dependOn(&run_cmd.step);
}

fn gitCommitHash(b: *std.Build) []const u8 {
    const result = std.process.run(b.allocator, b.graph.io, .{
        .argv = &.{ "git", "rev-parse", "HEAD" },
        .cwd = .{ .path = b.build_root.path orelse "." },
    }) catch return "<no git>";
    if (result.term != .exited or result.term.exited != 0) return "<no git>";
    return std.mem.trim(u8, result.stdout, " \n\r\t");
}

fn systemInfo(b: *std.Build) []const u8 {
    const result = std.process.run(b.allocator, b.graph.io, .{
        .argv = &.{ "uname", "-srvom" },
        .cwd = .{ .path = b.build_root.path orelse "." },
    }) catch return @tagName(@import("builtin").target.os.tag);
    if (result.term != .exited or result.term.exited != 0)
        return @tagName(@import("builtin").target.os.tag);
    return std.mem.trim(u8, result.stdout, " \n\r\t");
}

fn collectSources(b: *std.Build) []const []const u8 {
    var sources: std.ArrayList([]const u8) = .empty;

    var dir = b.build_root.handle.openDir(
        b.graph.io,
        "src",
        .{ .iterate = true },
    ) catch @panic("unable to open the src directory");
    defer dir.close(b.graph.io);

    var walker = dir.walk(b.allocator) catch @panic("out of memory");
    defer walker.deinit();

    while (walker.next(b.graph.io) catch @panic("unable to walk src")) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.path, ".cpp")) continue;
        if (std.mem.eql(u8, entry.path, "PlatformPosix.cpp")) continue;
        if (std.mem.eql(u8, entry.path, "PlatformPosixExtra.cpp")) continue;
        if (std.mem.eql(u8, entry.path, "PlatformWin32.cpp")) continue;
        sources.append(b.allocator, b.dupe(entry.path)) catch @panic("out of memory");
    }

    return sources.toOwnedSlice(b.allocator) catch @panic("out of memory");
}
