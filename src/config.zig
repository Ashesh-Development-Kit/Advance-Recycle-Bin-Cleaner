const std = @import("std");
const Config = @import("advance_recycle_bin_cleaner").Config;
const Handle = *anyopaque;
const invalid: Handle = @ptrFromInt(std.math.maxInt(usize));
extern "kernel32" fn GetModuleFileNameW(?Handle, [*]u16, u32) callconv(.winapi) u32;
extern "kernel32" fn CreateFileW([*:0]const u16, u32, u32, ?*anyopaque, u32, u32, ?Handle) callconv(.winapi) Handle;
extern "kernel32" fn ReadFile(Handle, [*]u8, u32, *u32, ?*anyopaque) callconv(.winapi) i32;
extern "kernel32" fn WriteFile(Handle, [*]const u8, u32, *u32, ?*anyopaque) callconv(.winapi) i32;
extern "kernel32" fn FlushFileBuffers(Handle) callconv(.winapi) i32;
extern "kernel32" fn CloseHandle(Handle) callconv(.winapi) i32;
extern "kernel32" fn MoveFileExW([*:0]const u16, [*:0]const u16, u32) callconv(.winapi) i32;
extern "kernel32" fn DeleteFileW([*:0]const u16) callconv(.winapi) i32;
extern "kernel32" fn GetLastError() callconv(.winapi) u32;
extern "kernel32" fn CreateMutexW(?*anyopaque, i32, [*:0]const u16) callconv(.winapi) ?Handle;
extern "kernel32" fn WaitForSingleObject(Handle, u32) callconv(.winapi) u32;
extern "kernel32" fn ReleaseMutex(Handle) callconv(.winapi) i32;
extern "kernel32" fn GetCurrentProcessId() callconv(.winapi) u32;
extern "kernel32" fn GetEnvironmentVariableW([*:0]const u16, [*]u16, u32) callconv(.winapi) u32;
extern "kernel32" fn CreateDirectoryW([*:0]const u16, ?*anyopaque) callconv(.winapi) i32;
extern "kernel32" fn GetTickCount64() callconv(.winapi) u64;
extern "kernel32" fn Sleep(u32) callconv(.winapi) void;

pub const Resolved = struct { settings: Config, path: [:0]const u16 };

pub fn resolve(allocator: std.mem.Allocator, portable: [:0]const u16, local_buffer: []u16) !Resolved {
    const mutex = try lock();
    defer unlock(mutex);
    const local = localPath(local_buffer) catch null;
    if (local) |path| {
        if (load(allocator, path)) |settings| return .{ .settings = settings, .path = path } else |err| {
            if (err != error.NotFound) return err;
        }
    }
    const existing = load(allocator, portable) catch |err| switch (err) {
        error.NotFound => null,
        else => return err,
    };
    const settings = existing orelse Config{};
    const portable_check = if (existing != null)
        checkWritable(allocator, portable)
    else
        write(allocator, portable, settings);
    portable_check catch |err| {
        if (err != error.ConfigDirectoryReadOnly) return err;
        const fallback = local orelse return error.LocalAppDataUnavailable;
        var dir_end = fallback.len;
        while (dir_end > 0 and fallback[dir_end - 1] != '\\') : (dir_end -= 1) {}
        local_buffer[dir_end - 1] = 0;
        const created = CreateDirectoryW(local_buffer[0 .. dir_end - 1 :0], null);
        local_buffer[dir_end - 1] = '\\';
        if (created == 0 and GetLastError() != 183) return error.ConfigDirectoryFailed;
        try write(allocator, fallback, settings);
        return .{ .settings = settings, .path = fallback };
    };
    return .{ .settings = settings, .path = portable };
}

fn localPath(buffer: []u16) ![:0]u16 {
    const n = GetEnvironmentVariableW(std.unicode.utf8ToUtf16LeStringLiteral("LOCALAPPDATA"), buffer.ptr, @intCast(buffer.len));
    if (n == 0 or n >= buffer.len) return error.LocalAppDataUnavailable;
    const suffix = std.unicode.utf8ToUtf16LeStringLiteral("\\AdvanceRecycleBinCleaner\\advance_recycle_bin_cleaner.json");
    if (n + suffix.len >= buffer.len) return error.PathTooLong;
    @memcpy(buffer[n..][0..suffix.len], suffix);
    buffer[n + suffix.len] = 0;
    return buffer[0 .. n + suffix.len :0];
}

pub fn configPath(buffer: []u16) ![:0]u16 {
    const n = GetModuleFileNameW(null, buffer.ptr, @intCast(buffer.len));
    if (n == 0 or n >= buffer.len) return error.ExecutablePathUnavailable;
    var end: usize = n;
    while (end > 0 and buffer[end - 1] != '\\') : (end -= 1) {}
    const name = std.unicode.utf8ToUtf16LeStringLiteral("advance_recycle_bin_cleaner.json");
    if (end + name.len >= buffer.len) return error.PathTooLong;
    @memcpy(buffer[end..][0..name.len], name);
    buffer[end + name.len] = 0;
    return buffer[0 .. end + name.len :0];
}

pub fn load(allocator: std.mem.Allocator, path: [*:0]const u16) !Config {
    const file = CreateFileW(path, 0x80000000, 1 | 4, null, 3, 0x80, null);
    if (file == invalid) {
        const code = GetLastError();
        if (code == 2 or code == 3) return error.NotFound;
        return error.ConfigOpenFailed;
    }
    defer _ = CloseHandle(file);
    var bytes: [16385]u8 = undefined;
    var count: u32 = 0;
    if (ReadFile(file, &bytes, bytes.len, &count, null) == 0) return error.ConfigReadFailed;
    if (count == bytes.len) return error.ConfigTooLarge;
    const parsed = std.json.parseFromSlice(Config, allocator, bytes[0..count], .{ .ignore_unknown_fields = true }) catch |err| switch (err) {
        error.OutOfMemory => return err,
        else => return error.MalformedConfiguration,
    };
    defer parsed.deinit();
    return parsed.value;
}

fn lock() !Handle {
    const mutex = CreateMutexW(null, 0, std.unicode.utf8ToUtf16LeStringLiteral("Local\\AdvanceRecycleBinCleanerConfig")) orelse return error.ConfigLockFailed;
    errdefer _ = CloseHandle(mutex);
    const wait = WaitForSingleObject(mutex, 30000);
    if (wait != 0 and wait != 0x80) return error.ConfigLockFailed;
    return mutex;
}

fn unlock(mutex: Handle) void {
    _ = ReleaseMutex(mutex);
    _ = CloseHandle(mutex);
}

pub fn update(allocator: std.mem.Allocator, path: [:0]const u16, previous: Config, next: Config) !void {
    const mutex = try lock();
    defer unlock(mutex);
    var current = try load(allocator, path);
    inline for (@typeInfo(Config).@"struct".fields) |field| {
        if (@field(previous, field.name) != @field(next, field.name))
            @field(current, field.name) = @field(next, field.name);
    }
    try write(allocator, path, current);
}

fn tempPath(allocator: std.mem.Allocator, path: [:0]const u16) ![:0]u16 {
    var suffix_buffer: [64]u8 = undefined;
    const suffix = try std.fmt.bufPrint(&suffix_buffer, ".{d}.{d}.tmp", .{ GetCurrentProcessId(), GetTickCount64() });
    const temp = try allocator.allocSentinel(u16, path.len + suffix.len, 0);
    @memcpy(temp[0..path.len], path);
    for (suffix, 0..) |char, i| temp[path.len + i] = char;
    return temp;
}

fn writableError(code: u32) error{ ConfigDirectoryReadOnly, ConfigWriteFailed } {
    return if (code == 5 or code == 19) error.ConfigDirectoryReadOnly else error.ConfigWriteFailed;
}

fn checkWritable(allocator: std.mem.Allocator, path: [:0]const u16) !void {
    const file = CreateFileW(path, 0x40010000, 1 | 2 | 4, null, 3, 0x80, null);
    if (file == invalid) return writableError(GetLastError());
    _ = CloseHandle(file);
    const temp = try tempPath(allocator, path);
    defer allocator.free(temp);
    const probe = CreateFileW(temp, 0x40010000, 0, null, 1, 0x04000080, null);
    if (probe == invalid) return writableError(GetLastError());
    _ = CloseHandle(probe);
}

fn write(allocator: std.mem.Allocator, path: [:0]const u16, config: Config) !void {
    const json = try std.json.Stringify.valueAlloc(allocator, config, .{ .whitespace = .indent_2 });
    defer allocator.free(json);
    const temp = try tempPath(allocator, path);
    defer allocator.free(temp);
    const file = CreateFileW(temp, 0x40000000, 0, null, 1, 0x80, null);
    if (file == invalid) return writableError(GetLastError());
    var count: u32 = 0;
    const ok = WriteFile(file, json.ptr, @intCast(json.len), &count, null) != 0 and count == json.len and FlushFileBuffers(file) != 0;
    _ = CloseHandle(file);
    if (!ok) {
        _ = DeleteFileW(temp);
        return error.ConfigWriteFailed;
    }
    // Readers outside the app can briefly block replacement by opening without FILE_SHARE_DELETE.
    var retries: u8 = 0;
    while (MoveFileExW(temp, path, 0x1 | 0x8) == 0) {
        const code = GetLastError();
        if ((code == 32 or code == 33) and retries < 10) {
            retries += 1;
            Sleep(15);
            continue;
        }
        _ = DeleteFileW(temp);
        if (code == 5 or code == 19) return error.ConfigDirectoryReadOnly;
        return error.ConfigReplaceFailed;
    }
}
