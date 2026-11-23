#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stddef.h>
#include "engine.h"
#include "log.h"

typedef struct {
    ULONG next, index;
    LARGE_INTEGER created, accessed, modified, changed, size, allocation;
    ULONG attributes, name_bytes;
    WCHAR name[256];
} DirectoryEntry;

typedef NTSTATUS (NTAPI *QueryDirectory)(HANDLE, HANDLE, PVOID, PVOID,
    PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS, BOOLEAN,
    PUNICODE_STRING, BOOLEAN);

static void fail(DriveResult *result, DWORD code) {
    if (!result->failures) result->first_error = code;
    if (result->failures != UINT32_MAX) ++result->failures;
}

static DWORD open_child(HANDLE parent, WCHAR *name, USHORT bytes, ACCESS_MASK access, HANDLE *out) {
    UNICODE_STRING component = { bytes, bytes, name };
    OBJECT_ATTRIBUTES object = {
        .Length = sizeof(object),
        .RootDirectory = parent,
        .ObjectName = &component,
        .Attributes = OBJ_CASE_INSENSITIVE | 0x1000 /* OBJ_DONT_REPARSE */
    };
    IO_STATUS_BLOCK io;
    NTSTATUS status = NtOpenFile(out, access | SYNCHRONIZE, &object, &io,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0x00200000 | 0x20);
    return status < 0 ? RtlNtStatusToDosError(status) : ERROR_SUCCESS;
}

static DWORD remove_handle(HANDLE handle) {
    /* Delete on close, including read-only entries on supporting file systems.
       This avoids changing attributes on other hard links to the same file. */
    DWORD flags = 0x1 | 0x10;
    IO_STATUS_BLOCK io;
    NTSTATUS status = NtSetInformationFile(handle, &io, &flags, sizeof(flags),
        (FILE_INFORMATION_CLASS)64);
    if (status >= 0) return ERROR_SUCCESS;
    DWORD code = RtlNtStatusToDosError(status);
    if (code != ERROR_INVALID_FUNCTION && code != ERROR_INVALID_PARAMETER &&
        code != ERROR_NOT_SUPPORTED && code != ERROR_INVALID_LEVEL) return code;
    FILE_DISPOSITION_INFO disposition = { TRUE };
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
        sizeof(disposition)) ? ERROR_SUCCESS : GetLastError();
}

static void add_count(uint64_t *value, uint64_t delta) {
    *value = UINT64_MAX - *value < delta ? UINT64_MAX : *value + delta;
}

static void walk(HANDLE directory, QueryDirectory query, unsigned mode, unsigned depth, DriveResult *result) {
    if (depth >= 128) {
        fail(result, ERROR_DIRECTORY);
        return;
    }
    for (;;) {
        DirectoryEntry entry;
        IO_STATUS_BLOCK io;
        NTSTATUS status = query(directory, NULL, NULL, NULL, &io, &entry,
            sizeof(entry), (FILE_INFORMATION_CLASS)1, TRUE, NULL, FALSE);
        if ((ULONG)status == 0x80000006) return;
        if (status < 0) {
            fail(result, RtlNtStatusToDosError(status));
            return;
        }
        if (!entry.name_bytes || entry.name_bytes > sizeof(entry.name) ||
            entry.name_bytes % sizeof(WCHAR) ||
            io.Information < offsetof(DirectoryEntry, name) + entry.name_bytes) {
            fail(result, ERROR_INVALID_DATA);
            return;
        }
        USHORT count = (USHORT)(entry.name_bytes / sizeof(WCHAR));
        if ((count == 1 && entry.name[0] == L'.') ||
            (count == 2 && entry.name[0] == L'.' && entry.name[1] == L'.')) continue;
        BOOL valid = TRUE;
        for (USHORT i = 0; i < count; ++i) {
            WCHAR c = entry.name[i];
            if (!c || c == L'\\' || c == L'/' || c == L':') valid = FALSE;
        }
        if (!valid) {
            fail(result, ERROR_INVALID_NAME);
            continue;
        }
        ACCESS_MASK access = FILE_READ_ATTRIBUTES;
        if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) access |= FILE_LIST_DIRECTORY;
        if (mode != ADK_SCAN) access |= DELETE;
        HANDLE child;
        DWORD code = open_child(directory, entry.name, (USHORT)entry.name_bytes, access, &child);
        if (code) {
            fail(result, code);
            continue;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes;
        if (!GetFileInformationByHandleEx(child, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
            fail(result, GetLastError());
            CloseHandle(child);
            continue;
        }
        if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            /* Leave reparse entries intact, including junctions and cloud placeholders. */
            fail(result, ERROR_REPARSE_POINT_ENCOUNTERED);
        } else if (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            uint32_t failures = result->failures;
            walk(child, query, mode, depth + 1, result);
            if (mode != ADK_SCAN && failures == result->failures &&
                (code = remove_handle(child)) != 0) fail(result, code);
        } else {
            FILE_STANDARD_INFO info;
            BOOL size_known = GetFileInformationByHandleEx(child, FileStandardInfo, &info, sizeof(info));
            if (!size_known) fail(result, GetLastError());
            if (mode != ADK_SCAN && (code = remove_handle(child)) != 0) {
                fail(result, code);
            } else {
                add_count(&result->items, 1);
                if (size_known && info.EndOfFile.QuadPart > 0)
                    add_count(&result->bytes, (uint64_t)info.EndOfFile.QuadPart);
            }
        }
        CloseHandle(child);
    }
}

DriveResult adk_tree(void *parent, const wchar_t *name, unsigned mode) {
    DriveResult result = {0};
    QueryDirectory query = (QueryDirectory)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile");
    if (!query) {
        fail(&result, ERROR_PROC_NOT_FOUND);
        return result;
    }
    ACCESS_MASK access = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY;
    if (mode == ADK_REMOVE) access |= DELETE;
    HANDLE root;
    DWORD code = open_child(parent, (WCHAR *)name, (USHORT)(lstrlenW(name) * sizeof(WCHAR)), access, &root);
    if (code) {
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) fail(&result, code);
        return result;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes;
    if (!GetFileInformationByHandleEx(root, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        fail(&result, GetLastError());
    } else if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        fail(&result, ERROR_REPARSE_POINT_ENCOUNTERED);
    } else if (!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fail(&result, ERROR_DIRECTORY);
    } else {
        walk(root, query, mode, 0, &result);
        if (mode == ADK_REMOVE && !result.failures && (code = remove_handle(root)) != 0)
            fail(&result, code);
    }
    CloseHandle(root);
    return result;
}

uint32_t adk_drives(void) {
    DWORD logical = GetLogicalDrives();
    uint32_t mask = 0;
    WCHAR path[] = L"A:\\";
    WCHAR volume[MAX_PATH];
    for (unsigned i = 0; i < 26; ++i) {
        if (!(logical & (1u << i))) continue;
        path[0] = (WCHAR)(L'A' + i);
        UINT type = GetDriveTypeW(path);
        if ((type == DRIVE_FIXED || type == DRIVE_REMOVABLE) &&
            GetVolumeNameForVolumeMountPointW(path, volume, ARRAYSIZE(volume))) mask |= 1u << i;
    }
    return mask;
}

int adk_is_elevated(void) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    TOKEN_ELEVATION elevation;
    DWORD length;
    BOOL elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length)
        && elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

static DriveResult process(unsigned index, unsigned mode) {
    WCHAR drive[] = L"A:\\";
    WCHAR volume[MAX_PATH];
    drive[0] = (WCHAR)(L'A' + index);
    DriveResult result = {0};
    if (!GetVolumeNameForVolumeMountPointW(drive, volume, ARRAYSIZE(volume))) {
        fail(&result, GetLastError());
        return result;
    }
    HANDLE parent = CreateFileW(volume, FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (parent == INVALID_HANDLE_VALUE) {
        fail(&result, GetLastError());
        return result;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes;
    ULARGE_INTEGER available;
    DWORD space_error = 0;
    BOOL has_before = GetDiskFreeSpaceExW(volume, &available, NULL, NULL);
    uint64_t before = has_before ? available.QuadPart : 0;
    if (!has_before) space_error = GetLastError();
    if (!GetFileInformationByHandleEx(parent, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        fail(&result, GetLastError());
    } else if (!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        fail(&result, ERROR_DIRECTORY);
    } else {
        result = adk_tree(parent, L"$Recycle.Bin", mode);
    }
    result.free_before_known = has_before;
    result.free_before = before;
    if (space_error) fail(&result, space_error);
    if (mode != ADK_SCAN) {
        if (GetDiskFreeSpaceExW(volume, &available, NULL, NULL)) {
            result.free_after_known = TRUE;
            result.free_after = available.QuadPart;
        } else fail(&result, GetLastError());
    }
    CloseHandle(parent);
    return result;
}

void adk_run(uint32_t mask, unsigned mode, Progress progress, void *context) {
    const wchar_t *action = mode == ADK_SCAN ? L"Scan"
        : mode == ADK_KEEP ? L"Cleanup (keep folder)" : L"Cleanup (remove folder)";
    unsigned total = 0, done = 0;
    uint64_t failures = 0;
    for (unsigned i = 0; i < 26; ++i) total += (mask >> i) & 1u;
    adk_logf(0, L"%ls started for %u drive(s).", action, total);
    for (unsigned i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        progress(context, i, done, total, NULL);
        DriveResult result = process(i, mode);
        failures += result.failures;
        adk_logf(result.failures != 0, L"%ls %lc: %llu file(s), %llu logical byte(s), %u failure(s), first Windows error %u.",
            action, L'A' + i, (unsigned long long)result.items, (unsigned long long)result.bytes,
            (unsigned)result.failures, (unsigned)result.first_error);
        progress(context, i, ++done, total, &result);
    }
    adk_logf(0, L"%ls finished, %llu failure(s).", action, (unsigned long long)failures);
}
