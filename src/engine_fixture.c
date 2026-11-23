#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stddef.h>
#include <stdio.h>
#include "engine.h"

static BOOL make_file(const WCHAR *path, DWORD attributes) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written;
    BOOL ok = WriteFile(file, "abc", 3, &written, NULL) && written == 3;
    CloseHandle(file);
    return ok && SetFileAttributesW(path, attributes);
}

static BOOL junction(const WCHAR *path, const WCHAR *target) {
    struct {
        DWORD tag;
        WORD data_length, reserved, offset, length, display_offset, display_length;
        WCHAR buffer[MAX_PATH + 8];
    } data = {0};
    if (!CreateDirectoryW(path, NULL)) return FALSE;
    if (swprintf(data.buffer, ARRAYSIZE(data.buffer), L"\\??\\%ls", target) < 0) return FALSE;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.length = lstrlenW(data.buffer) * sizeof(WCHAR);
    data.display_offset = data.length + sizeof(WCHAR);
    data.data_length = 8 + data.display_offset + sizeof(WCHAR);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD returned;
    BOOL ok = DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, &data, 8 + data.data_length,
        NULL, 0, &returned, NULL);
    CloseHandle(file);
    return ok;
}

static BOOL absent(const WCHAR *path) {
    return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}

int adk_fixture(void) {
    WCHAR temp[MAX_PATH], base[MAX_PATH], bin[MAX_PATH], nested[MAX_PATH], readonly[MAX_PATH];
    WCHAR locked[MAX_PATH], outside[MAX_PATH], sentinel[MAX_PATH], link[MAX_PATH], root_link[MAX_PATH];
    if (!GetTempPathW(ARRAYSIZE(temp), temp) || !GetTempFileNameW(temp, L"adk", 0, base)) return __LINE__;
    if (!DeleteFileW(base) || !CreateDirectoryW(base, NULL)) return __LINE__;
    if (swprintf(bin, ARRAYSIZE(bin), L"%ls\\bin", base) < 0 ||
        swprintf(nested, ARRAYSIZE(nested), L"%ls\\nested", bin) < 0 ||
        swprintf(readonly, ARRAYSIZE(readonly), L"%ls\\read-only", nested) < 0 ||
        swprintf(locked, ARRAYSIZE(locked), L"%ls\\locked", bin) < 0 ||
        swprintf(outside, ARRAYSIZE(outside), L"%ls\\outside", base) < 0 ||
        swprintf(sentinel, ARRAYSIZE(sentinel), L"%ls\\sentinel", outside) < 0 ||
        swprintf(link, ARRAYSIZE(link), L"%ls\\junction", bin) < 0 ||
        swprintf(root_link, ARRAYSIZE(root_link), L"%ls\\root-link", base) < 0) return __LINE__;

    int failure = 0;
    HANDLE parent = INVALID_HANDLE_VALUE, lock = INVALID_HANDLE_VALUE;
    DriveResult result = {0};
#define CHECK(condition) do { if (!(condition)) { failure = __LINE__; goto done; } } while (0)
    CHECK(CreateDirectoryW(bin, NULL));
    CHECK(CreateDirectoryW(nested, NULL));
    CHECK(CreateDirectoryW(outside, NULL));
    CHECK(make_file(readonly, FILE_ATTRIBUTE_READONLY));
    CHECK(make_file(locked, FILE_ATTRIBUTE_NORMAL));
    CHECK(make_file(sentinel, FILE_ATTRIBUTE_NORMAL));
    CHECK(junction(link, outside));
    CHECK(junction(root_link, outside));
    parent = CreateFileW(base, FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    CHECK(parent != INVALID_HANDLE_VALUE);

    result = adk_tree(parent, L"root-link", ADK_REMOVE);
    CHECK(result.failures == 1 && result.items == 0 && !absent(sentinel));
    result = adk_tree(parent, L"bin", ADK_SCAN);
    CHECK(result.failures == 1 && result.items == 2 && result.bytes == 6);
    lock = CreateFileW(locked, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    result = adk_tree(parent, L"bin", ADK_KEEP);
    CHECK(result.failures == 2 && result.items == 1 && result.bytes == 3);
    CHECK(absent(nested) && !absent(locked) && !absent(sentinel) && !absent(link));
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;
    CHECK(RemoveDirectoryW(link));
    result = adk_tree(parent, L"bin", ADK_KEEP);
    CHECK(result.failures == 0 && result.items == 1 && result.bytes == 3);
    CHECK(absent(locked) && !absent(bin));
    CHECK(make_file(locked, FILE_ATTRIBUTE_NORMAL));
    result = adk_tree(parent, L"bin", ADK_REMOVE);
    CHECK(result.failures == 0 && result.items == 1 && absent(bin));
    result = adk_tree(parent, L"bin", ADK_SCAN);
    CHECK(result.failures == 0 && result.items == 0);
done:
    if (failure) fprintf(stderr, "Fixture failed at line %d (Win32 %lu, operation %u, %u errors, %llu files)\n",
        failure, GetLastError(), result.first_error, result.failures, (unsigned long long)result.items);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    if (parent != INVALID_HANDLE_VALUE) CloseHandle(parent);
    RemoveDirectoryW(root_link);
    RemoveDirectoryW(link);
    SetFileAttributesW(readonly, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(readonly);
    DeleteFileW(locked);
    RemoveDirectoryW(nested);
    RemoveDirectoryW(bin);
    DeleteFileW(sentinel);
    RemoveDirectoryW(outside);
    RemoveDirectoryW(base);
    return failure;
#undef CHECK
}

int adk_startup_fixture(void) {
    WCHAR sandbox[128];
    swprintf(sandbox, ARRAYSIZE(sandbox), L"Software\\ADK-Startup-Test-%lu-%llu",
        GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    HKEY run;
    DWORD disposition;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox, 0, NULL, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, NULL, &run, &disposition) != ERROR_SUCCESS) return __LINE__;
    if (disposition != REG_CREATED_NEW_KEY) {
        RegCloseKey(run);
        return __LINE__;
    }
    int error = 0;
    WCHAR command[MAX_PATH], actual[MAX_PATH];
    DWORD bytes = sizeof(actual), type = 0;
#define CHECK(condition) do { if (!(condition)) { error = __LINE__; goto done; } } while (0)
    CHECK(adk_startup_value(run, TRUE) == ERROR_SUCCESS);
    CHECK(RegQueryValueExW(run, L"AdvanceRecycleBinCleaner", NULL, &type, (BYTE *)actual, &bytes) == ERROR_SUCCESS);
    command[0] = L'"';
    DWORD length = GetModuleFileNameW(NULL, command + 1, ARRAYSIZE(command) - 16);
    CHECK(length && length < ARRAYSIZE(command) - 16);
    lstrcpyW(command + 1 + length, L"\" --minimized");
    CHECK(type == REG_SZ && lstrcmpW(command, actual) == 0);
    CHECK(adk_startup_value(run, FALSE) == ERROR_SUCCESS);
    CHECK(RegQueryValueExW(run, L"AdvanceRecycleBinCleaner", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND);
done:
    RegCloseKey(run);
    RegDeleteKeyW(HKEY_CURRENT_USER, sandbox);
    return error;
#undef CHECK
}
