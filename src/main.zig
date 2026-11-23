const std = @import("std");
const core = @import("advance_recycle_bin_cleaner");
const config = @import("config.zig");
const engine = @import("engine.zig");

extern fn adk_desktop(scope: u32, minimized: c_int, settings: *const [setting_count]i32) c_int;
extern fn adk_notify_quick(failed: c_int) void;
extern fn adk_log(is_error: c_int, message: [*:0]const u16) void;
extern fn adk_is_elevated() c_int;
extern fn adk_request_desktop_admin(scope: u32, minimized: c_int) u32;
extern "kernel32" fn AttachConsole(u32) callconv(.winapi) i32;
extern "kernel32" fn GetStdHandle(u32) callconv(.winapi) ?*anyopaque;
extern "user32" fn MessageBoxW(?*anyopaque, [*:0]const u16, [*:0]const u16, u32) callconv(.winapi) i32;

const setting_count = @typeInfo(core.Config).@"struct".fields.len;
var active_config: config.Resolved = undefined;

fn logError(context: []const u8, err: anyerror) void {
    var raw: [256]u8 = undefined;
    const text = std.fmt.bufPrint(&raw, "{s}: {s}", .{ context, @errorName(err) }) catch return;
    var wide: [256]u16 = undefined;
    const count = std.unicode.utf8ToUtf16Le(&wide, text) catch return;
    wide[count] = 0;
    adk_log(1, wide[0..count :0]);
}

fn useExistingConsole() void {
    // Respect redirection and reuse a parent terminal when available.
    const stderr = GetStdHandle(@bitCast(@as(i32, -12)));
    if (stderr == null or @intFromPtr(stderr.?) == std.math.maxInt(usize))
        _ = AttachConsole(std.math.maxInt(u32));
}

fn showDesktopError(err: anyerror) void {
    var message: [256]u8 = undefined;
    const text = std.fmt.bufPrint(&message, "Could not start Advance Recycle Bin Cleaner.\n\n{s}", .{@errorName(err)}) catch return;
    var wide_message: [256]u16 = undefined;
    const count = std.unicode.utf8ToUtf16Le(&wide_message, text) catch return;
    wide_message[count] = 0;
    _ = MessageBoxW(null, wide_message[0..count :0], std.unicode.utf8ToUtf16LeStringLiteral("Advance Recycle Bin Cleaner"), 0x10);
}

fn pack(settings: core.Config) [setting_count]i32 {
    var values: [setting_count]i32 = undefined;
    inline for (@typeInfo(core.Config).@"struct".fields, 0..) |field, index| {
        const value = @field(settings, field.name);
        values[index] = if (field.type == bool) @intFromBool(value) else value;
    }
    return values;
}

export fn adk_save(values: *const [setting_count]i32) c_int {
    var next: core.Config = undefined;
    inline for (@typeInfo(core.Config).@"struct".fields, 0..) |field, index| {
        @field(next, field.name) = if (field.type == bool) values[index] != 0 else values[index];
    }
    config.update(std.heap.page_allocator, active_config.path, active_config.settings, next) catch |err| {
        logError("Could not save settings", err);
        std.debug.print("Could not save configuration: {s}\n", .{@errorName(err)});
        return 0;
    };
    active_config.settings = next;
    return 1;
}

fn usage() void {
    std.debug.print(
        \\Advance Recycle Bin Cleaner
        \\Usage: ARBC.exe [--widget | --scan | --quick-clean] [--drive C,D]
        \\--widget       Open the desktop widget (default).
        \\--minimized    Start the widget hidden in the notification area.
        \\--scan         Report recycle bin counts and available free space.
        \\--quick-clean  Permanently clean selected recycle bins without prompting.
        \\--keep-folder  Keep the bin root during quick-clean regardless of settings.
        \\--drive C,D    Select drives for scan or cleanup, or one initial widget drive.
        \\--help         Show this help.
        \\
    , .{});
}

const Report = struct {
    failed: bool = false,
    clean: bool,
};

fn printResult(context: ?*anyopaque, index: c_uint, _: c_uint, _: c_uint, maybe_result: ?*const engine.DriveResult) callconv(.c) void {
    const result = maybe_result orelse return;
    const state: *Report = @ptrCast(@alignCast(context.?));
    const letter: u8 = 'A' + @as(u8, @intCast(index));
    std.debug.print("{c}: {d} bytes, {d} files {s}", .{ letter, result.bytes, result.items, if (state.clean) "removed" else "counted" });
    if (result.free_before_known != 0) std.debug.print(", {d} bytes free", .{result.free_before});
    if (result.free_after_known != 0) std.debug.print(" -> {d} bytes free", .{result.free_after});
    if (result.failures != 0) {
        state.failed = true;
        std.debug.print(", partial result: {d} failure(s), first Windows error {d}", .{ result.failures, result.first_error });
    }
    std.debug.print("\n", .{});
}

pub fn main(init: std.process.Init) !void {
    const allocator = init.arena.allocator();
    const args = try init.minimal.args.toSlice(allocator);
    const from_elevation = args.len > 1 and std.mem.eql(u8, args[1], "--elevated-widget");
    const start: usize = if (from_elevation) 2 else 1;
    const options = core.parseOptions(args[start..]) catch |err| {
        logError("Invalid command line", err);
        useExistingConsole();
        usage();
        return err;
    };
    if (from_elevation and (options.mode != .widget or options.help)) return error.InvalidDesktopLaunch;
    if (options.help or options.mode != .widget) useExistingConsole();
    if (options.help) {
        usage();
        return;
    }
    if (options.mode == .widget and adk_is_elevated() == 0) {
        if (from_elevation) {
            logError("Desktop launch", error.AdministratorPrivilegesRequired);
            showDesktopError(error.AdministratorPrivilegesRequired);
            return error.AdministratorPrivilegesRequired;
        }
        const status = adk_request_desktop_admin(options.drives orelse 0, @intFromBool(options.minimized));
        if (status == 0 or status == 1223) return; // Success or user canceled UAC.
        showDesktopError(error.AdministratorLaunchFailed);
        return error.AdministratorLaunchFailed;
    }
    adk_log(0, std.unicode.utf8ToUtf16LeStringLiteral("ARBC started."));
    errdefer adk_log(0, std.unicode.utf8ToUtf16LeStringLiteral("ARBC exited with an error."));
    const available = engine.adk_drives();
    const target = options.drives orelse available;
    if (target & ~available != 0) {
        logError("Requested drive unavailable", error.DriveNotAvailable);
        if (options.mode == .widget) showDesktopError(error.DriveNotAvailable);
        return error.DriveNotAvailable;
    }
    var path_buffer: [32768]u16 = undefined;
    var local_buffer: [32768]u16 = undefined;
    const path = config.configPath(&path_buffer) catch |err| {
        logError("Could not resolve settings path", err);
        if (options.mode == .widget) showDesktopError(err);
        return err;
    };
    active_config = config.resolve(allocator, path, &local_buffer) catch |err| {
        logError("Could not load settings", err);
        if (options.mode == .widget) showDesktopError(err) else std.debug.print("Could not load configuration: {s}\n", .{@errorName(err)});
        return err;
    };
    const settings = active_config.settings;
    if (options.mode == .widget) {
        adk_log(0, std.unicode.utf8ToUtf16LeStringLiteral("Desktop started with administrator privileges."));
        const values = pack(settings);
        if (adk_desktop(options.drives orelse 0, @intFromBool(options.minimized), &values) != 0) {
            logError("Desktop failed", error.DesktopFailed);
            showDesktopError(error.DesktopFailed);
            return error.DesktopFailed;
        }
        adk_log(0, std.unicode.utf8ToUtf16LeStringLiteral("Desktop closed."));
        return;
    }
    if (target == 0) {
        logError("No supported drives", error.NoSupportedDrives);
        return error.NoSupportedDrives;
    }
    var state: Report = .{ .clean = options.mode == .clean };
    const mode: engine.Mode = if (!state.clean) .scan else if (options.keep_folder or !settings.delete_root_folder) .keep else .remove;
    engine.adk_run(target, @intFromEnum(mode), printResult, &state);
    if (state.clean) {
        const cleanup_failed = state.failed;
        state.clean = false;
        std.debug.print("Post-cleanup scan\n", .{});
        engine.adk_run(target, @intFromEnum(engine.Mode.scan), printResult, &state);
        if (settings.notifications) adk_notify_quick(@intFromBool(cleanup_failed));
    }
    if (state.failed) return error.OperationIncomplete;
    adk_log(0, std.unicode.utf8ToUtf16LeStringLiteral("ARBC finished."));
}
