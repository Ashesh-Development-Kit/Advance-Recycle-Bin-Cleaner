#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "log.h"

enum { LOG_LIMIT = 1024 * 1024 };

static BOOL ensure_directory(const WCHAR *path) {
    if (CreateDirectoryW(path, NULL)) return TRUE;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void append_line(WCHAR *path, size_t capacity, const char *line, DWORD length) {
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart + length > LOG_LIMIT) {
        CloseHandle(file);
        size_t end = lstrlenW(path);
        if (end + 2 >= capacity) return;
        path[end] = L'.';
        path[end + 1] = L'1';
        path[end + 2] = 0;
        WCHAR previous[32768];
        lstrcpyW(previous, path);
        path[end] = 0;
        if (!MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING)) return;
        file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    DWORD written;
    WriteFile(file, line, length, &written, NULL);
    CloseHandle(file);
}

void adk_log(int error, const wchar_t *message) {
    WCHAR path[32768];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    const WCHAR directory[] = L"\\AdvanceRecycleBinCleaner";
    if (!length || length >= ARRAYSIZE(path) - 80) return;
    lstrcpyW(path + length, directory);
    if (!ensure_directory(path)) return;
    lstrcatW(path, L"\\Logs");
    if (!ensure_directory(path)) return;
    size_t directory_end = lstrlenW(path);

    SYSTEMTIME time;
    GetSystemTime(&time);
    WCHAR wide_line[512];
    if (swprintf(wide_line, ARRAYSIZE(wide_line), L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%ls] %ls\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, error ? L"ERROR" : L"INFO", message) < 0) return;
    char line[2048];
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_line, -1,
        line, sizeof(line), NULL, NULL);
    if (count <= 1) return;

    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\AdvanceRecycleBinCleanerLog");
    if (!mutex) return;
    DWORD wait = WaitForSingleObject(mutex, 250);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        lstrcpyW(path + directory_end, L"\\ARBC.log");
        append_line(path, ARRAYSIZE(path), line, (DWORD)count - 1);
        if (error) {
            lstrcpyW(path + directory_end, L"\\ARBC-error.log");
            append_line(path, ARRAYSIZE(path), line, (DWORD)count - 1);
        }
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
}

void adk_logf(int error, const wchar_t *format, ...) {
    WCHAR message[400];
    va_list args;
    va_start(args, format);
    int length = vswprintf(message, ARRAYSIZE(message), format, args);
    va_end(args);
    if (length >= 0 && (size_t)length < ARRAYSIZE(message)) adk_log(error, message);
}
