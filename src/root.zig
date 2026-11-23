const std = @import("std");

/// Portable or per-user settings shared by the desktop and CLI.
pub const Config = struct {
    x: i32 = 120,
    y: i32 = 120,
    run_on_startup: bool = false,
    auto_scan: bool = true,
    confirm_delete: bool = true,
    notifications: bool = true,
    delete_root_folder: bool = true,
    click_through: bool = false,
    hide_to_tray: bool = false,
    minimize_to_tray: bool = true,
    exit_to_tray: bool = true,
    show_on_taskbar: bool = false,
    position_saved: bool = false,
    opacity: i32 = 100,
};

test "configuration defaults and JSON decoding" {
    const parsed = try std.json.parseFromSlice(Config, std.testing.allocator, "{\"x\":42,\"click_through\":true}", .{});
    defer parsed.deinit();
    try std.testing.expectEqual(@as(i32, 42), parsed.value.x);
    try std.testing.expect(parsed.value.click_through);
    try std.testing.expect(parsed.value.confirm_delete);
    try std.testing.expectEqual(@as(i32, 120), parsed.value.y);
}

pub const Options = struct {
    pub const Mode = enum { widget, scan, clean };
    mode: Mode = .widget,
    drives: ?u32 = null,
    help: bool = false,
    minimized: bool = false,
    keep_folder: bool = false,
};

pub fn parseOptions(args: []const []const u8) !Options {
    var options: Options = .{};
    var mode_seen = false;
    var i: usize = 0;
    while (i < args.len) : (i += 1) {
        const arg = args[i];
        if (std.mem.eql(u8, arg, "--help")) {
            options.help = true;
        } else if (std.mem.eql(u8, arg, "--scan") or std.mem.eql(u8, arg, "--quick-clean") or std.mem.eql(u8, arg, "--widget")) {
            if (mode_seen) return error.DuplicateMode;
            mode_seen = true;
            options.mode = if (std.mem.eql(u8, arg, "--quick-clean"))
                .clean
            else if (std.mem.eql(u8, arg, "--scan"))
                .scan
            else
                .widget;
        } else if (std.mem.eql(u8, arg, "--minimized")) {
            options.minimized = true;
        } else if (std.mem.eql(u8, arg, "--keep-folder")) {
            options.keep_folder = true;
        } else if (std.mem.eql(u8, arg, "--drive")) {
            if (options.drives != null or i + 1 >= args.len) return error.InvalidDriveList;
            i += 1;
            options.drives = try parseDriveList(args[i]);
        } else return error.UnknownArgument;
    }
    if (!mode_seen and options.drives != null and !options.minimized) options.mode = .scan;
    if (options.keep_folder and options.mode != .clean) return error.KeepFolderRequiresQuickClean;
    if (options.minimized and options.mode != .widget) return error.MinimizedRequiresWidget;
    if (options.mode == .widget and options.drives != null and @popCount(options.drives.?) != 1)
        return error.WidgetRequiresSingleDrive;
    return options;
}

pub fn parseDriveList(value: []const u8) !u32 {
    var mask: u32 = 0;
    var iter = std.mem.splitScalar(u8, value, ',');
    while (iter.next()) |part| {
        if (part.len != 1 or !std.ascii.isAlphabetic(part[0])) return error.InvalidDriveList;
        const index: u5 = @intCast(std.ascii.toUpper(part[0]) - 'A');
        mask |= @as(u32, 1) << index;
    }
    return mask;
}

test "command line modes and selection" {
    const options = try parseOptions(&.{ "--quick-clean", "--drive", "c,D" });
    try std.testing.expectEqual(Options.Mode.clean, options.mode);
    try std.testing.expectEqual(@as(?u32, 12), options.drives);
    try std.testing.expectError(error.DuplicateMode, parseOptions(&.{ "--scan", "--quick-clean" }));
    try std.testing.expectError(error.InvalidDriveList, parseOptions(&.{"--drive"}));
    try std.testing.expectError(error.InvalidDriveList, parseOptions(&.{ "--drive", "C", "--drive", "D" }));
    try std.testing.expectError(error.KeepFolderRequiresQuickClean, parseOptions(&.{"--keep-folder"}));
    try std.testing.expectError(error.MinimizedRequiresWidget, parseOptions(&.{ "--scan", "--minimized" }));
    try std.testing.expectEqual(Options.Mode.widget, (try parseOptions(&.{})).mode);
    try std.testing.expectEqual(Options.Mode.scan, (try parseOptions(&.{ "--drive", "C" })).mode);
    try std.testing.expect((try parseOptions(&.{ "--quick-clean", "--keep-folder" })).keep_folder);
}

test "drive lists accept letters and reject malformed input" {
    try std.testing.expectEqual(@as(u32, 0b1100), try parseDriveList("c,D"));
    for ([_][]const u8{ "", "C,", "C:", "CD", "1" }) |bad| {
        try std.testing.expectError(error.InvalidDriveList, parseDriveList(bad));
    }
}
