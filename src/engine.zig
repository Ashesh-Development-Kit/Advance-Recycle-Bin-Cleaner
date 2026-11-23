pub const Mode = enum(c_uint) { scan, keep, remove };

pub const DriveResult = extern struct {
    bytes: u64,
    items: u64,
    free_before: u64,
    free_after: u64,
    failures: u32,
    first_error: u32,
    free_before_known: u32,
    free_after_known: u32,
};

const Progress = *const fn (?*anyopaque, c_uint, c_uint, c_uint, ?*const DriveResult) callconv(.c) void;
pub extern fn adk_drives() u32;
pub extern fn adk_run(mask: u32, mode: c_uint, progress: Progress, context: ?*anyopaque) void;
pub extern fn adk_tree(parent: *anyopaque, name: [*:0]const u16, mode: c_uint) DriveResult;
