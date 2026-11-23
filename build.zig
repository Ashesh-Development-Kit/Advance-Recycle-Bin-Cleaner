const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const core = b.addModule("advance_recycle_bin_cleaner", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const run_step = b.step("run", "Run the application");
    for ([_][]const u8{ "ARBC", "advance_recycle_bin_cleaner_cli" }, 0..) |name, index| {
        const cli = index != 0;
        const app = b.addExecutable(.{
            .name = name,
            .root_module = b.createModule(.{
                .root_source_file = b.path("src/main.zig"),
                .target = target,
                .optimize = optimize,
                .link_libc = true,
                .imports = &.{.{ .name = "advance_recycle_bin_cleaner", .module = core }},
            }),
        });
        app.subsystem = if (cli) .console else .windows;
        for ([_][]const u8{ "kernel32", "shell32", "user32", "advapi32", "gdi32", "comctl32", "ntdll", "ole32" }) |library|
            app.root_module.linkSystemLibrary(library, .{});
        app.root_module.addCSourceFiles(.{
            .files = &.{ "src/engine.c", "src/desktop.c", "src/log.c" },
            .flags = &.{ "-std=c11", "-Wall", "-Wextra" },
        });
        app.root_module.addWin32ResourceFile(.{
            .file = b.path("app.rc"),
            .flags = if (cli) &.{"/DARBC_CLI"} else &.{},
        });
        b.installArtifact(app);
        if (!cli) {
            const run = b.addRunArtifact(app);
            if (b.args) |args| run.addArgs(args);
            run_step.dependOn(&run.step);
        }
    }
    const tests = b.addTest(.{ .root_module = core });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_tests.step);
    const fixtures = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/engine_tests.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{.{ .name = "advance_recycle_bin_cleaner", .module = core }},
    }) });
    fixtures.root_module.linkSystemLibrary("kernel32", .{});
    fixtures.root_module.linkSystemLibrary("ntdll", .{});
    for ([_][]const u8{ "advapi32", "gdi32", "comctl32", "shell32", "user32", "ole32" }) |library|
        fixtures.root_module.linkSystemLibrary(library, .{});
    fixtures.root_module.addCSourceFiles(.{
        .files = &.{ "src/engine.c", "src/desktop.c", "src/log.c", "src/engine_fixture.c" },
        .flags = &.{ "-std=c11", "-Wall", "-Wextra" },
    });
    test_step.dependOn(&b.addRunArtifact(fixtures).step);
}
