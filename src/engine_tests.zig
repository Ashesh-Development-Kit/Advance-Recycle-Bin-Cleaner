const std = @import("std");
const config = @import("config.zig");
const Config = @import("advance_recycle_bin_cleaner").Config;
extern fn adk_fixture() c_int;
extern fn adk_startup_fixture() c_int;

export fn adk_save(_: [*]const i32) c_int {
    return 0;
}

test "scan and cleanup are confined to the disposable tree" {
    try std.testing.expectEqual(@as(c_int, 0), adk_fixture());
}

test "startup registration adds and removes the quoted command in an isolated registry key" {
    try std.testing.expectEqual(@as(c_int, 0), adk_startup_fixture());
}

test "settings preserve independent updates and refuse malformed files" {
    const allocator = std.testing.allocator;
    const io = std.testing.io;
    var temp = std.testing.tmpDir(.{});
    defer temp.cleanup();
    try temp.dir.writeFile(io, .{ .sub_path = "settings.json", .data = "{}" });
    const utf8 = try temp.dir.realPathFileAlloc(io, "settings.json", allocator);
    defer allocator.free(utf8);
    const path = try std.unicode.utf8ToUtf16LeAllocZ(allocator, utf8);
    defer allocator.free(path);
    const baseline = try config.load(allocator, path);
    try std.testing.expectEqual(Config{}, baseline);

    var widget_update = baseline;
    widget_update.x = 300;
    try config.update(allocator, path, baseline, widget_update);
    var settings_update = baseline;
    settings_update.auto_scan = false;
    try config.update(allocator, path, baseline, settings_update);
    const merged = try config.load(allocator, path);
    try std.testing.expectEqual(@as(i32, 300), merged.x);
    try std.testing.expect(!merged.auto_scan);

    try temp.dir.writeFile(io, .{ .sub_path = "settings.json", .data = "{" });
    try std.testing.expectError(error.MalformedConfiguration, config.update(allocator, path, merged, baseline));
    try std.testing.expectError(error.MalformedConfiguration, config.load(allocator, path));
}
