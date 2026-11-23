#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dbt.h>
#include <objbase.h>
#include <wchar.h>
#include <limits.h>
#include "engine.h"
#include "log.h"

enum { X, Y, STARTUP, AUTO_SCAN, CONFIRM, NOTIFY, REMOVE_ROOT, CLICK_THROUGH,
       HIDE, MINIMIZE, EXIT_TO_TRAY, SHOW_ON_TASKBAR, POSITION_SAVED, OPACITY, SETTING_COUNT };
enum { SCAN = 100, CLEAN_SELECTED, CLEAN_ALL, RESTORE, SETTINGS, HIDE_WIDGET, CLICK, QUIT, DRIVE, CLOSE_APP, SNAP, MINIMIZE_WIDGET };
enum { CHECK_BASE = 200, TRAY_MESSAGE = WM_APP + 1, TIMER = 1 };
enum { CLEAN_DRIVE_BASE = 300 };
enum { WIDGET_WIDTH = 250, HEADER_HEIGHT = 30, ROW_HEIGHT = 22, FOOTER_HEIGHT = 27, OLD_DEFAULT = 120 };
static const WCHAR *labels[] = {
    L"Run on Windows startup", L"Scan on launch", L"Confirm before cleaning",
    L"Show cleanup notifications", L"Delete the $Recycle.Bin folder",
    L"Click-through widget", L"Hide widget to tray", L"Minimize settings to tray",
    L"Close to tray", L"Show widget on taskbar"
};

typedef struct {
    uint32_t mask, refresh_mask;
    unsigned mode;
    unsigned total, cleanup_total;
    volatile LONG progress, current, refreshing;
    DriveResult scans[26], cleanup[26];
} Operation;

static struct {
    HINSTANCE instance;
    HWND widget, settings, status, list, drives, bar, checks[SETTING_COUNT];
    HWND opacity_slider, opacity_label;
    HANDLE worker;
    HICON icon;
    UINT taskbar_created;
    BOOL tray, closing, confirming, drag, moved, configuring;
    int32_t config[SETTING_COUNT];
    POINT drag_start;
    RECT origin;
    DriveResult results[26], cleaned[26];
    uint32_t mask, visible_mask, scanned_mask, cleaned_mask, changed_during_job;
    int selected, pressed_button;
    int rows, first_row, wheel_delta;
    int opacity;
    const WCHAR *cleanup_status;
    unsigned animation;
    double displayed;
    Operation job;
} app;

static void layout_widget(void);

static void warning(const WCHAR *message) {
    adk_log(1, message);
    MessageBoxW(IsWindowVisible(app.settings) ? app.settings : app.widget, message,
        L"Advance Recycle Bin Cleaner", MB_OK | MB_ICONWARNING | MB_TASKMODAL);
}

uint32_t adk_startup_value(void *handle, int enabled) {
    const WCHAR *name = L"AdvanceRecycleBinCleaner";
    LSTATUS code;
    if (enabled) {
        WCHAR command[32768];
        command[0] = L'"';
        DWORD length = GetModuleFileNameW(NULL, command + 1, ARRAYSIZE(command) - 16);
        if (!length || length >= ARRAYSIZE(command) - 16 || length + 14 >= 260) {
            code = ERROR_FILENAME_EXCED_RANGE;
        } else {
            memcpy(command + 1 + length, L"\" --minimized", sizeof(L"\" --minimized"));
            code = RegSetValueExW(handle, name, 0, REG_SZ, (const BYTE *)command,
                (lstrlenW(command) + 1) * sizeof(WCHAR));
        }
    } else {
        code = RegDeleteValueW(handle, name);
        if (code == ERROR_FILE_NOT_FOUND) code = ERROR_SUCCESS;
    }
    return code;
}

static uint32_t adk_startup(int enabled) {
    const WCHAR *key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY handle;
    LSTATUS code = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &handle, NULL);
    if (code != ERROR_SUCCESS) return code;
    uint32_t result = adk_startup_value(handle, enabled);
    RegCloseKey(handle);
    return result;
}
static NOTIFYICONDATAW tray_data(HWND hwnd, HICON icon) {
    NOTIFYICONDATAW data = {0};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = TRAY_MESSAGE;
    data.hIcon = icon;
    lstrcpynW(data.szTip, L"Advance Recycle Bin Cleaner", ARRAYSIZE(data.szTip));
    return data;
}

static BOOL add_tray(void) {
    NOTIFYICONDATAW data = tray_data(app.widget, app.icon);
    app.tray = Shell_NotifyIconW(NIM_ADD, &data) || Shell_NotifyIconW(NIM_MODIFY, &data);
    if (!app.tray) adk_log(1, L"Windows did not accept the notification area icon.");
    return app.tray;
}

static BOOL notify(HWND hwnd, BOOL failed) {
    NOTIFYICONDATAW data = tray_data(hwnd, NULL);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = failed ? NIIF_WARNING : NIIF_INFO;
    lstrcpynW(data.szInfoTitle, L"Recycle Bin cleanup", ARRAYSIZE(data.szInfoTitle));
    lstrcpynW(data.szInfo, failed ? L"Cleanup completed with errors. See ARBC-error.log." : L"Cleanup completed.", ARRAYSIZE(data.szInfo));
    BOOL sent = Shell_NotifyIconW(NIM_MODIFY, &data);
    if (!sent) adk_log(1, L"Windows did not accept the cleanup notification.");
    return sent;
}

void adk_notify_quick(int failed) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!owner) return;
    NOTIFYICONDATAW data = tray_data(owner, LoadIconW(instance, MAKEINTRESOURCEW(1)));
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        if (notify(owner, failed)) Sleep(4000);
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    DestroyWindow(owner);
}

static BOOL apply_opacity(int percent) {
    BYTE alpha = (BYTE)((255 * percent + 50) / 100);
    if (!SetLayeredWindowAttributes(app.widget, RGB(255, 0, 255), alpha, LWA_COLORKEY | LWA_ALPHA))
        return FALSE;
    app.opacity = percent;
    WCHAR label[40];
    swprintf(label, ARRAYSIZE(label), L"Widget opacity: %d%%", percent);
    SetWindowTextW(app.opacity_label, label);
    return TRUE;
}

static void apply_widget_style(void) {
    LONG_PTR style = GetWindowLongPtrW(app.widget, GWL_EXSTYLE);
    BOOL through = app.tray && app.config[CLICK_THROUGH];
    BOOL taskbar = app.config[SHOW_ON_TASKBAR] || !app.tray;
    LONG_PTR next = style & ~(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW | WS_EX_TRANSPARENT);
    next |= taskbar ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
    if (through) next |= WS_EX_TRANSPARENT;
    if (style == next) return;

    // The shell reevaluates taskbar eligibility when the window is shown again.
    BOOL reshow = ((style ^ next) & (WS_EX_TOOLWINDOW | WS_EX_APPWINDOW)) && IsWindowVisible(app.widget);
    BOOL minimized = IsIconic(app.widget);
    if (reshow) ShowWindow(app.widget, SW_HIDE);
    SetWindowLongPtrW(app.widget, GWL_EXSTYLE, next);
    if (reshow) ShowWindow(app.widget, minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
}

static void sync_checks(void) {
    if (!app.settings) return;
    for (int i = STARTUP; i < POSITION_SAVED; ++i)
        SendMessageW(app.checks[i], BM_SETCHECK, app.config[i] ? BST_CHECKED : BST_UNCHECKED, 0);
}

static BOOL save(void) {
    if (adk_save(app.config)) return TRUE;
    warning(L"Windows could not save the settings. Check that the settings file is writable and valid.");
    return FALSE;
}

static void change_opacity(BOOL commit) {
    int percent = (int)SendMessageW(app.opacity_slider, TBM_GETPOS, 0, 0);
    if (percent != app.opacity && !apply_opacity(percent)) {
        SendMessageW(app.opacity_slider, TBM_SETPOS, TRUE, app.opacity);
        warning(L"Windows could not change widget opacity.");
        return;
    }
    if (!commit || percent == app.config[OPACITY]) return;
    int previous = app.config[OPACITY];
    app.config[OPACITY] = percent;
    if (!save()) {
        app.config[OPACITY] = previous;
        apply_opacity(previous);
        SendMessageW(app.opacity_slider, TBM_SETPOS, TRUE, previous);
    }
}

static void change_setting(int setting, int value) {
    if (app.config[setting] == value) return;
    int previous = app.config[setting];
    if (setting == STARTUP && adk_startup(value)) {
        warning(L"Windows could not update the startup registration.");
        sync_checks();
        return;
    }
    app.config[setting] = value;
    if (!save()) {
        app.config[setting] = previous;
        if (setting == STARTUP && adk_startup(previous))
            warning(L"Windows could not restore the previous startup registration.");
    }
    sync_checks();
    apply_widget_style();
    if (setting == HIDE && app.tray)
        ShowWindow(app.widget, app.config[HIDE] ? SW_HIDE : SW_SHOWNOACTIVATE);
}

static void show_results(void) {
    InvalidateRect(app.widget, NULL, FALSE);
    if (!app.settings) return;
    app.configuring = TRUE;
    SendMessageW(app.drives, CB_RESETCONTENT, 0, 0);
    SendMessageW(app.drives, CB_ADDSTRING, 0, (LPARAM)L"All drives");
    SendMessageW(app.drives, CB_SETITEMDATA, 0, -1);
    SendMessageW(app.drives, CB_SETCURSEL, 0, 0);
    SendMessageW(app.list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 26; ++i) {
        if (!(app.mask & (1u << i))) continue;
        WCHAR drive[] = { (WCHAR)(L'A' + i), L':', 0 };
        LRESULT item = SendMessageW(app.drives, CB_ADDSTRING, 0, (LPARAM)drive);
        SendMessageW(app.drives, CB_SETITEMDATA, item, i);
        if (app.selected == i) SendMessageW(app.drives, CB_SETCURSEL, item, 0);
        if (!(app.scanned_mask & (1u << i))) continue;
        DriveResult *result = &app.results[i];
        WCHAR line[256], space[50];
        if (result->free_before_known)
            swprintf(space, ARRAYSIZE(space), L"%llu B free", (unsigned long long)result->free_before);
        else lstrcpyW(space, L"Free space unavailable");
        swprintf(line, ARRAYSIZE(line), L"%ls   %llu B / %llu files   %ls",
            drive, (unsigned long long)result->bytes, (unsigned long long)result->items, space);
        SendMessageW(app.list, LB_ADDSTRING, 0, (LPARAM)line);
        if (result->failures) {
            swprintf(line, ARRAYSIZE(line), L"     Partial scan: %lu failure(s), first Windows error %lu",
                (unsigned long)result->failures, (unsigned long)result->first_error);
            SendMessageW(app.list, LB_ADDSTRING, 0, (LPARAM)line);
        }
        if (app.cleaned_mask & (1u << i)) {
            DriveResult *clean = &app.cleaned[i];
            swprintf(line, ARRAYSIZE(line), L"     Cleanup: %llu B of files removed",
                (unsigned long long)clean->bytes);
            SendMessageW(app.list, LB_ADDSTRING, 0, (LPARAM)line);
            if (clean->free_before_known && clean->free_after_known) {
                swprintf(line, ARRAYSIZE(line), L"     Free space: %llu B -> %llu B",
                    (unsigned long long)clean->free_before, (unsigned long long)clean->free_after);
                SendMessageW(app.list, LB_ADDSTRING, 0, (LPARAM)line);
            }
            if (clean->failures) {
                swprintf(line, ARRAYSIZE(line), L"     Cleanup: %lu failure(s), first Windows error %lu",
                    (unsigned long)clean->failures, (unsigned long)clean->first_error);
                SendMessageW(app.list, LB_ADDSTRING, 0, (LPARAM)line);
            }
        }
    }
    app.configuring = FALSE;
}

static unsigned count_drives(uint32_t mask) {
    unsigned count = 0;
    for (; mask; mask &= mask - 1) ++count;
    return count;
}

static void update_drives(void) {
    uint32_t previous = app.mask;
    app.visible_mask = GetLogicalDrives() & 0x3ffffffu;
    app.mask = adk_drives() & app.visible_mask;
    if (app.worker) app.changed_during_job |= previous ^ app.mask;
    app.scanned_mask &= app.mask;
    app.cleaned_mask &= app.mask;
    if (app.selected >= 0 && !(app.mask & (1u << app.selected))) app.selected = -1;
    layout_widget();
    show_results();
}

static void report(void *context, unsigned index, unsigned done, unsigned total, const DriveResult *result) {
    Operation *job = context;
    (void)total;
    if (result) {
        DriveResult *results = job->mode == ADK_SCAN ? job->scans : job->cleanup;
        results[index] = *result;
    }
    InterlockedExchange(&job->current, index);
    InterlockedExchange(&job->progress, done);
}

static void refresh(void *context, unsigned index, unsigned done, unsigned total, const DriveResult *result) {
    Operation *job = context;
    (void)total;
    if (result) job->scans[index] = *result;
    InterlockedExchange(&job->current, index);
    InterlockedExchange(&job->progress, job->cleanup_total + done);
}

uint32_t adk_request_desktop_admin(uint32_t scope, int minimized) {
    WCHAR exe[32768], args[80];
    DWORD length = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!length || length >= ARRAYSIZE(exe)) {
        adk_log(1, L"Could not find the desktop executable for startup elevation.");
        return ERROR_BAD_PATHNAME;
    }
    WCHAR *name = wcsrchr(exe, L'\\');
    if (name && lstrcmpiW(name + 1, L"advance_recycle_bin_cleaner_cli.exe") == 0)
        lstrcpyW(name + 1, L"ARBC.exe");
    lstrcpyW(args, L"--elevated-widget --widget");
    if (minimized) lstrcatW(args, L" --minimized");
    if (scope) {
        WCHAR drive_option[] = L" --drive A";
        for (unsigned i = 0; i < 26; ++i) {
            if (scope != (1u << i)) continue;
            drive_option[ARRAYSIZE(drive_option) - 2] = (WCHAR)(L'A' + i);
            lstrcatW(args, drive_option);
            break;
        }
    }
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW launch = {0};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas";
    launch.lpFile = exe;
    launch.lpParameters = args;
    launch.nShow = SW_SHOWNORMAL;
    adk_log(0, L"Requesting administrator approval at desktop startup.");
    BOOL started = ShellExecuteExW(&launch);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (SUCCEEDED(com)) CoUninitialize();
    if (started) return ERROR_SUCCESS;
    if (error == ERROR_CANCELLED) adk_log(0, L"Desktop startup canceled at administrator approval.");
    else adk_logf(1, L"Could not start the desktop as administrator. Windows error %lu.", error);
    return error ? error : ERROR_GEN_FAILURE;
}

static DWORD WINAPI operation(void *context) {
    Operation *job = context;
    adk_run(job->mask, job->mode, report, job);
    if (job->mode != ADK_SCAN) {
        InterlockedExchange(&job->refreshing, 1);
        adk_run(job->refresh_mask, ADK_SCAN, refresh, job);
    }
    return 0;
}

static void begin(BOOL clean, int selection) {
    if (app.worker || app.closing || app.confirming) return;
    update_drives();
    uint32_t mask = app.mask;
    uint32_t target = selection < 0 ? mask : mask & (1u << selection);
    if (!target) {
        warning(L"No supported drive is available for this operation.");
        return;
    }
    if (clean && app.config[CONFIRM]) {
        WCHAR message[256], scope[32] = L"all supported drives";
        if (selection >= 0)
            swprintf(scope, ARRAYSIZE(scope), L"drive %lc:", L'A' + selection);
        swprintf(message, ARRAYSIZE(message), L"Permanently remove recycle bin %ls on %ls?",
            app.config[REMOVE_ROOT] ? L"folders and their contents" : L"contents", scope);
        app.confirming = TRUE;
        int answer = MessageBoxW(IsWindowVisible(app.settings) ? app.settings : app.widget,
            message, L"Confirm cleanup", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING | MB_TASKMODAL);
        app.confirming = FALSE;
        if (answer != IDYES || app.closing || !IsWindow(app.widget)) {
            adk_log(0, L"Cleanup canceled at confirmation.");
            return;
        }
    }
    memset(&app.job, 0, sizeof(app.job));
    app.cleanup_status = NULL;
    app.changed_during_job = 0;
    app.job.mode = clean ? (app.config[REMOVE_ROOT] ? ADK_REMOVE : ADK_KEEP) : ADK_SCAN;
    app.job.mask = target;
    app.job.refresh_mask = mask;
    app.job.cleanup_total = clean ? count_drives(target) : 0;
    app.job.total = clean ? app.job.cleanup_total + count_drives(mask) : count_drives(target);
    app.displayed = 0;
    app.animation = 0;
    if (!SetTimer(app.widget, TIMER, 30, NULL)) {
        warning(L"Windows could not start operation progress.");
        return;
    }
    app.worker = CreateThread(NULL, 0, operation, &app.job, 0, NULL);
    if (!app.worker) {
        KillTimer(app.widget, TIMER);
        warning(L"Windows could not start the operation.");
    }
    InvalidateRect(app.widget, NULL, FALSE);
}

static unsigned progress_percent(void) {
    unsigned current = (unsigned)InterlockedCompareExchange(&app.job.progress, 0, 0);
    return app.job.total ? 100 * current / app.job.total : 0;
}

static const WCHAR *operation_label(void) {
    if (app.job.mode == ADK_SCAN) return L"Scanning";
    return InterlockedCompareExchange(&app.job.refreshing, 0, 0) ? L"Refreshing" : L"Cleaning";
}

static void format_operation(WCHAR *text, size_t length, unsigned target) {
    LONG drive = InterlockedCompareExchange(&app.job.current, 0, 0);
    swprintf(text, length, L"%ls %lc: %u%%", operation_label(), L'A' + drive, target);
}

static void update_operation(void) {
    if (!app.worker) return;
    if (WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) {
        CloseHandle(app.worker);
        app.worker = NULL;
        KillTimer(app.widget, TIMER);
        if (app.closing) {
            DestroyWindow(app.widget);
            return;
        }
        memcpy(app.results, app.job.scans, sizeof(app.results));
        uint32_t current_drives = app.mask & ~app.changed_during_job;
        app.scanned_mask = (app.job.mode == ADK_SCAN ? app.job.mask : app.job.refresh_mask) & current_drives;
        BOOL failed = FALSE;
        if (app.job.mode != ADK_SCAN) {
            app.cleaned_mask = app.job.mask & current_drives;
            memcpy(app.cleaned, app.job.cleanup, sizeof(app.cleaned));
            for (int i = 0; i < 26; ++i) if (app.cleaned[i].failures) failed = TRUE;
            app.cleanup_status = failed ? L"Cleanup errors. See ARBC-error.log." : L"Cleanup complete";
            if (app.config[NOTIFY] && app.tray && !app.closing) notify(app.widget, failed);
        }
        BOOL partial = FALSE;
        for (int i = 0; i < 26; ++i) if (app.results[i].failures) partial = TRUE;
        SetWindowTextW(app.status, failed ? L"Cleanup completed with errors. See ARBC-error.log."
            : partial ? L"Scan completed with partial results." : L"Ready");
        SendMessageW(app.bar, PBM_SETPOS, 0, 0);
        show_results();
        return;
    }
    unsigned target = progress_percent();
    app.displayed += ((double)target - app.displayed) * 0.22;
    ++app.animation;
    WCHAR status[96];
    format_operation(status, ARRAYSIZE(status), target);
    WCHAR previous[96];
    if (app.status && (!GetWindowTextW(app.status, previous, ARRAYSIZE(previous)) || lstrcmpW(previous, status)))
        SetWindowTextW(app.status, status);
    if (SendMessageW(app.bar, PBM_GETPOS, 0, 0) != (LRESULT)app.displayed)
        SendMessageW(app.bar, PBM_SETPOS, (WPARAM)app.displayed, 0);
    RECT footer;
    if (GetClientRect(app.widget, &footer)) {
        footer.top = footer.bottom - FOOTER_HEIGHT;
        InvalidateRect(app.widget, &footer, FALSE);
    }
}

static int drive_at_row(int row) {
    if (row < 0 || row >= app.rows) return -1;
    row += app.first_row;
    for (int i = 0; i < 26; ++i) {
        if ((app.visible_mask & (1u << i)) && row-- == 0) return i;
    }
    return -1;
}

static void select_row(int y) {
    if (y < HEADER_HEIGHT) app.selected = -1;
    else {
        int drive = drive_at_row((y - HEADER_HEIGHT) / ROW_HEIGHT);
        if (drive < 0 || !(app.mask & (1u << drive))) return;
        app.selected = app.selected == drive ? -1 : drive;
    }
    show_results();
}

static void restore(void) {
    if (app.config[HIDE]) change_setting(HIDE, FALSE);
    ShowWindow(app.widget, IsIconic(app.widget) ? SW_RESTORE : SW_SHOWNOACTIVATE);
    update_drives();
}

static void minimize_widget(void) {
    if (app.tray && app.config[MINIMIZE]) ShowWindow(app.widget, SW_HIDE);
    else ShowWindow(app.widget, SW_MINIMIZE);
}

static void close_app(BOOL explicit_quit) {
    if (!explicit_quit && app.config[EXIT_TO_TRAY] && app.tray) {
        ShowWindow(app.widget, SW_HIDE);
        ShowWindow(app.settings, SW_HIDE);
    } else if (app.worker) {
        app.closing = TRUE;
        ShowWindow(app.widget, SW_HIDE);
        ShowWindow(app.settings, SW_HIDE);
    } else DestroyWindow(app.widget);
}

static void open_settings(void);
static void command(unsigned id) {
    if (app.closing || app.confirming) return;
    switch (id) {
    case SCAN: begin(FALSE, -1); break;
    case CLEAN_SELECTED: begin(TRUE, app.selected); break;
    case CLEAN_ALL: begin(TRUE, -1); break;
    case RESTORE: restore(); break;
    case SETTINGS: open_settings(); break;
    case HIDE_WIDGET: if (app.tray) change_setting(HIDE, TRUE); break;
    case CLICK: if (app.tray) change_setting(CLICK_THROUGH, !app.config[CLICK_THROUGH]); break;
    case QUIT: close_app(TRUE); break;
    case CLOSE_APP: close_app(FALSE); break;
    case MINIMIZE_WIDGET: minimize_widget(); break;
    case SNAP: {
        int x = app.config[X], y = app.config[Y], saved = app.config[POSITION_SAVED];
        app.config[X] = app.config[Y] = OLD_DEFAULT;
        app.config[POSITION_SAVED] = FALSE;
        if (!save()) {
            app.config[X] = x;
            app.config[Y] = y;
            app.config[POSITION_SAVED] = saved;
        }
        restore();
        break;
    }
    }
}

static void menu(BOOL from_tray) {
    POINT point;
    if (!GetCursorPos(&point)) return;
    HMENU popup = CreatePopupMenu();
    if (!popup) return;
    AppendMenuW(popup, MF_STRING, RESTORE, L"Restore widget");
    AppendMenuW(popup, MF_STRING, SNAP, L"Snap above taskbar");
    AppendMenuW(popup, MF_STRING, SETTINGS, L"Settings...");
    AppendMenuW(popup, MF_SEPARATOR, 0, NULL);
    UINT working = app.worker ? MF_GRAYED : MF_ENABLED;
    AppendMenuW(popup, MF_STRING | working, SCAN, L"Rescan drives");
    AppendMenuW(popup, MF_STRING | working, CLEAN_SELECTED, L"Clean selected view");
    AppendMenuW(popup, MF_STRING | working, CLEAN_ALL, L"Clean all drives");
    AppendMenuW(popup, MF_SEPARATOR, 0, NULL);
    UINT tray = app.tray ? MF_ENABLED : MF_GRAYED;
    AppendMenuW(popup, MF_STRING | tray, HIDE_WIDGET, L"Hide widget to tray");
    AppendMenuW(popup, MF_STRING | tray | (app.config[CLICK_THROUGH] ? MF_CHECKED : 0), CLICK, L"Click-through widget");
    AppendMenuW(popup, MF_STRING, from_tray ? QUIT : CLOSE_APP,
        from_tray ? L"Quit application" : L"Close application");
    SetForegroundWindow(app.widget);
    TrackPopupMenu(popup, TPM_RIGHTBUTTON,
        point.x, point.y, 0, app.widget, NULL);
    PostMessageW(app.widget, WM_NULL, 0, 0);
    DestroyMenu(popup);
}

static void format_size(WCHAR *text, size_t capacity, uint64_t bytes) {
    const WCHAR *units[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB" };
    double size = (double)bytes;
    unsigned unit = 0;
    while (size >= 1024 && unit + 1 < ARRAYSIZE(units)) {
        size /= 1024;
        ++unit;
    }
    if (unit) swprintf(text, capacity, L"%.2f %ls", size, units[unit]);
    else swprintf(text, capacity, L"%llu B", (unsigned long long)bytes);
}

static void row_text(HDC dc, const WCHAR *text, int left, int right, int top, UINT align) {
    RECT box = {left, top, right, top + ROW_HEIGHT};
    DrawTextW(dc, text, -1, &box, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static RECT button_bounds(RECT frame, int row) {
    int top = HEADER_HEIGHT + row * ROW_HEIGHT;
    return (RECT){frame.right - 62, top + 2, frame.right - 10, top + ROW_HEIGHT - 2};
}

static RECT header_button_bounds(RECT frame, int action) {
    if (action == CLOSE_APP) return (RECT){frame.right - 30, 4, frame.right - 8, 26};
    if (action == MINIMIZE_WIDGET) return (RECT){frame.right - 56, 4, frame.right - 34, 26};
    return (RECT){frame.right - 130, 4, frame.right - 60, 26};
}

static int button_at(HWND hwnd, POINT point) {
    RECT frame;
    if (!GetClientRect(hwnd, &frame)) return 0;
    const int header[] = {CLEAN_ALL, MINIMIZE_WIDGET, CLOSE_APP};
    for (unsigned i = 0; i < ARRAYSIZE(header); ++i) {
        RECT button = header_button_bounds(frame, header[i]);
        if (PtInRect(&button, point)) return header[i];
    }
    if (point.y < HEADER_HEIGHT) return 0;
    int row = (point.y - HEADER_HEIGHT) / ROW_HEIGHT;
    int drive = drive_at_row(row);
    if (drive < 0) return 0;
    RECT button = button_bounds(frame, row);
    return PtInRect(&button, point) ? CLEAN_DRIVE_BASE + drive : 0;
}

static BOOL button_enabled(int action) {
    if (app.closing || app.confirming) return FALSE;
    if (action == CLOSE_APP || action == MINIMIZE_WIDGET) return TRUE;
    if (app.worker) return FALSE;
    return action == CLEAN_ALL ? app.mask != 0 : (app.mask & (1u << (action - CLEAN_DRIVE_BASE))) != 0;
}

static void draw_button(HDC dc, RECT bounds, int action, const WCHAR *label) {
    BOOL enabled = button_enabled(action);
    COLORREF background = !enabled ? RGB(30, 63, 85)
        : app.pressed_button == action ? RGB(49, 119, 116) : RGB(30, 94, 106);
    HGDIOBJ brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, background);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, 6, 6);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
    SetTextColor(dc, enabled ? RGB(229, 252, 247) : RGB(136, 162, 180));
    row_text(dc, label, bounds.left + 3, bounds.right - 3,
        (bounds.top + bounds.bottom - ROW_HEIGHT) / 2, DT_CENTER);
}

static void paint_widget(HDC dc, RECT frame) {
    SetDCBrushColor(dc, RGB(255, 0, 255));
    FillRect(dc, &frame, GetStockObject(DC_BRUSH));
    HGDIOBJ brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, RGB(18, 48, 74));
    RoundRect(dc, 0, 0, frame.right, frame.bottom, 14, 14);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    COLORREF white = RGB(232, 248, 247), mint = RGB(67, 217, 190), muted = RGB(170, 200, 216);
    SetTextColor(dc, white);
    row_text(dc, L"RECYCLE BIN", 12, frame.right - 134, 4, DT_LEFT);
    draw_button(dc, header_button_bounds(frame, CLEAN_ALL), CLEAN_ALL, L"Clean all");
    draw_button(dc, header_button_bounds(frame, MINIMIZE_WIDGET), MINIMIZE_WIDGET, L"\x2212");
    draw_button(dc, header_button_bounds(frame, CLOSE_APP), CLOSE_APP, L"\x00D7");

    int row = 0, shown = 0;
    uint64_t failures = 0;
    for (int i = 0; i < 26; ++i) {
        uint32_t bit = 1u << i;
        if (!(app.visible_mask & bit)) continue;
        const DriveResult *result = &app.results[i];
        if (app.scanned_mask & bit) failures += result->failures;
        if (row++ < app.first_row || shown >= app.rows) continue;
        RECT button = button_bounds(frame, shown);
        int top = HEADER_HEIGHT + ROW_HEIGHT * shown++;
        RECT background = {8, top, frame.right - 8, top + ROW_HEIGHT - 1};
        SetDCBrushColor(dc, i == app.selected ? RGB(34, 83, 101) : RGB(26, 60, 87));
        FillRect(dc, &background, GetStockObject(DC_BRUSH));
        WCHAR letter[] = { (WCHAR)(L'A' + i), L':', 0 };
        SetTextColor(dc, app.mask & bit ? white : muted);
        row_text(dc, letter, 14, 38, top, DT_LEFT);
        if (!(app.mask & bit)) {
            row_text(dc, L"Unsupported drive", 40, button.left - 6, top, DT_LEFT);
        } else if (!(app.scanned_mask & bit)) {
            SetTextColor(dc, muted);
            row_text(dc, L"Awaiting scan", 40, button.left - 6, top, DT_LEFT);
        } else if (result->failures && !result->free_before_known && !result->items) {
            SetTextColor(dc, muted);
            row_text(dc, L"Scan unavailable *", 40, button.left - 6, top, DT_LEFT);
        } else {
            WCHAR bytes[32], items[44];
            format_size(bytes, ARRAYSIZE(bytes), result->bytes);
            swprintf(items, ARRAYSIZE(items), L"%llu files%ls",
                (unsigned long long)result->items, result->failures ? L" *" : L"");
            SetTextColor(dc, mint);
            row_text(dc, bytes, 40, 111, top, DT_LEFT);
            SetTextColor(dc, white);
            row_text(dc, items, 111, button.left - 6, top, DT_RIGHT);
        }
        draw_button(dc, button, CLEAN_DRIVE_BASE + i, L"Clean");
    }
    if (!app.visible_mask) {
        SetTextColor(dc, muted);
        row_text(dc, L"No drives detected", 12, frame.right - 12, HEADER_HEIGHT, DT_LEFT);
    }

    int footer = frame.bottom - FOOTER_HEIGHT;
    WCHAR status[96];
    if (app.worker) {
        format_operation(status, ARRAYSIZE(status), progress_percent());
    } else if (app.cleanup_status) {
        lstrcpyW(status, app.cleanup_status);
    } else if (failures) {
        lstrcpyW(status, L"* Partial results");
    } else {
        lstrcpyW(status, app.scanned_mask ? L"Scan complete" : L"Right-click to scan");
    }
    SetTextColor(dc, muted);
    BOOL clipped = count_drives(app.visible_mask) > (unsigned)app.rows;
    row_text(dc, status, 12, clipped ? 173 : frame.right - 12, footer, DT_LEFT);
    if (clipped) row_text(dc, L"Scroll for more", 173, frame.right - 12, footer, DT_RIGHT);
    if (app.worker) {
        RECT track = {12, frame.bottom - 6, frame.right - 12, frame.bottom - 3};
        int track_width = track.right - track.left;
        SetDCBrushColor(dc, RGB(49, 88, 113));
        FillRect(dc, &track, GetStockObject(DC_BRUSH));
        track.right = track.left + (long)(track_width * app.displayed / 100);
        if (track.right == track.left) {
            track.left += (app.animation % 25) * (track_width - 22) / 24;
            track.right = track.left + 22;
        }
        SetDCBrushColor(dc, mint);
        FillRect(dc, &track, GetStockObject(DC_BRUSH));
    }
    SelectObject(dc, font);
}

static void draw(HWND hwnd) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(hwnd, &paint);
    RECT frame;
    if (GetClientRect(hwnd, &frame) && !IsRectEmpty(&paint.rcPaint)) {
        // Compose the color-key background and all content off-screen.
        // Present only the invalidated pixels, in one copy.
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, frame.right, frame.bottom) : NULL;
        HGDIOBJ previous = bitmap ? SelectObject(buffer, bitmap) : NULL;
        if (previous && previous != HGDI_ERROR) {
            paint_widget(buffer, frame);
            RECT dirty = paint.rcPaint;
            BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                buffer, dirty.left, dirty.top, SRCCOPY);
            SelectObject(buffer, previous);
        } else paint_widget(dc, frame);
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
    }
    EndPaint(hwnd, &paint);
}

static int clamp(int value, int low, int high) {
    if (value < low || high < low) return low;
    if (value > high) return high;
    return value;
}

static RECT widget_bounds(POINT desired, BOOL snapped) {
    HWND taskbar = snapped ? FindWindowW(L"Shell_TrayWnd", NULL) : NULL;
    HMONITOR display = snapped ? MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY)
        : MonitorFromPoint(desired, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    RECT work;
    if (GetMonitorInfoW(display, &monitor)) work = monitor.rcWork;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        work = (RECT){0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    int width = clamp(work.right - work.left, 1, WIDGET_WIDTH);
    int count = (int)count_drives(app.visible_mask);
    app.rows = clamp(count, 1, (work.bottom - work.top - HEADER_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT);
    app.first_row = clamp(app.first_row, 0, count - app.rows);
    int height = HEADER_HEIGHT + ROW_HEIGHT * app.rows + FOOTER_HEIGHT;
    int x = snapped ? work.right - width : clamp(desired.x, work.left, work.right - width);
    int y = snapped ? work.bottom - height : clamp(desired.y, work.top, work.bottom - height);
    return (RECT){x, y, x + width, y + height};
}

static RECT configured_bounds(void) {
    // Older settings files used (120, 120) for an untouched position.
    BOOL snapped = !app.config[POSITION_SAVED] && app.config[X] == OLD_DEFAULT && app.config[Y] == OLD_DEFAULT;
    return widget_bounds((POINT){app.config[X], app.config[Y]}, snapped);
}

static void reveal_selection(void) {
    if (app.selected < 0) return;
    int row = (int)count_drives(app.visible_mask & ((1u << app.selected) - 1));
    if (row < app.first_row) app.first_row = row;
    if (row >= app.first_row + app.rows) app.first_row = row - app.rows + 1;
}

static void layout_widget(void) {
    if (!app.widget || app.drag || IsIconic(app.widget)) return;
    RECT frame = configured_bounds();
    SetWindowPos(app.widget, NULL, frame.left, frame.top, frame.right - frame.left,
        frame.bottom - frame.top, SWP_NOACTIVATE | SWP_NOZORDER);
    InvalidateRect(app.widget, NULL, FALSE);
}
static LRESULT CALLBACK widget_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message && message == app.taskbar_created) {
        if (!add_tray()) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        apply_widget_style();
        layout_widget();
        return 0;
    }
    switch (message) {
    case WM_PAINT: draw(hwnd); return 0;
    case WM_PRINTCLIENT: {
        RECT frame;
        if (GetClientRect(hwnd, &frame)) paint_widget((HDC)wp, frame);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: if (wp == TIMER) update_operation(); return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) layout_widget();
        return 0;
    case WM_DISPLAYCHANGE: layout_widget(); return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED)
            update_drives();
        return 0;
    case WM_MOUSEWHEEL: {
        app.wheel_delta += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = app.wheel_delta / WHEEL_DELTA;
        app.wheel_delta %= WHEEL_DELTA;
        app.first_row = clamp(app.first_row - steps, 0, (int)count_drives(app.visible_mask) - app.rows);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CONTEXTMENU: menu(FALSE); return 0;
    case WM_COMMAND: command(LOWORD(wp)); return 0;
    case TRAY_MESSAGE:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) menu(TRUE);
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) restore();
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int button = button_at(hwnd, point);
        if (button) {
            if (button_enabled(button)) {
                SetCapture(hwnd);
                if (GetCapture() == hwnd) app.pressed_button = button;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (GetCursorPos(&app.drag_start) && GetWindowRect(hwnd, &app.origin)) {
            app.drag = TRUE;
            app.moved = FALSE;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (app.drag) {
            POINT point;
            if (!GetCursorPos(&point)) return 0;
            int dx = point.x - app.drag_start.x, dy = point.y - app.drag_start.y;
            if (app.moved || dx >= 4 || dx <= -4 || dy >= 4 || dy <= -4) {
                app.moved = TRUE;
                SetWindowPos(hwnd, NULL, app.origin.left + dx, app.origin.top + dy, 0, 0,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (app.pressed_button) {
            int action = app.pressed_button;
            app.pressed_button = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (action == button_at(hwnd, point) && button_enabled(action)) {
                if (action >= CLEAN_DRIVE_BASE) begin(TRUE, action - CLEAN_DRIVE_BASE);
                else command(action);
            }
            return 0;
        }
        if (app.drag) {
            app.drag = FALSE;
            ReleaseCapture();
            if (app.moved) {
                RECT rect;
                if (GetWindowRect(hwnd, &rect)) {
                    RECT bounds = widget_bounds((POINT){rect.left, rect.top}, FALSE);
                    int x = app.config[X], y = app.config[Y], saved = app.config[POSITION_SAVED];
                    app.config[X] = bounds.left;
                    app.config[Y] = bounds.top;
                    app.config[POSITION_SAVED] = TRUE;
                    if (!save()) {
                        app.config[X] = x;
                        app.config[Y] = y;
                        app.config[POSITION_SAVED] = saved;
                    }
                    layout_widget();
                }
            } else select_row(GET_Y_LPARAM(lp));
        }
        return 0;
    case WM_CAPTURECHANGED:
        app.drag = FALSE;
        if (app.pressed_button) {
            app.pressed_button = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_MINIMIZE && app.tray && app.config[MINIMIZE]) {
            minimize_widget();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && app.tray && app.config[MINIMIZE]) ShowWindow(hwnd, SW_HIDE);
        return DefWindowProcW(hwnd, message, wp, lp);
    case WM_CLOSE: close_app(FALSE); return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW data = tray_data(hwnd, app.icon);
        if (app.tray) Shell_NotifyIconW(NIM_DELETE, &data);
        if (app.settings) DestroyWindow(app.settings);
        PostQuitMessage(0);
        return 0;
    }
    default: return DefWindowProcW(hwnd, message, wp, lp);
    }
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CLOSE:
        if (app.config[EXIT_TO_TRAY] && app.tray) {
            ShowWindow(hwnd, SW_HIDE);
            ShowWindow(app.widget, SW_HIDE);
        } else close_app(FALSE);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && app.config[MINIMIZE] && app.tray) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == app.opacity_slider)
            change_opacity(LOWORD(wp) != TB_THUMBTRACK);
        return 0;
    case WM_COMMAND: {
        unsigned id = LOWORD(wp);
        if (id >= CHECK_BASE + STARTUP && id < CHECK_BASE + POSITION_SAVED && HIWORD(wp) == BN_CLICKED) {
            int index = id - CHECK_BASE;
            change_setting(index, SendMessageW(app.checks[index], BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == DRIVE && HIWORD(wp) == CBN_SELCHANGE && !app.configuring) {
            app.selected = (int)SendMessageW(app.drives, CB_GETITEMDATA,
                SendMessageW(app.drives, CB_GETCURSEL, 0, 0), 0);
            reveal_selection();
            InvalidateRect(app.widget, NULL, FALSE);
        } else if (HIWORD(wp) == BN_CLICKED) command(id);
        return 0;
    }
    default: return DefWindowProcW(hwnd, message, wp, lp);
    }
}

static HWND control(const WCHAR *class_name, const WCHAR *text, DWORD style, int x, int y,
    int width, int height, unsigned id) {
    HWND child = CreateWindowExW(0, class_name, text, WS_VISIBLE | WS_CHILD | style,
        x, y, width, height, app.settings, (HMENU)(UINT_PTR)id, app.instance, NULL);
    SendMessageW(child, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return child;
}

static void open_settings(void) {
    if (!app.settings) {
        RECT bounds = {0, 0, 540, 545};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectEx(&bounds, style, FALSE, 0);
        app.settings = CreateWindowExW(0, L"ADKRecycleSettings", L"Advance Recycle Bin Cleaner Settings",
            style, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            NULL, NULL, app.instance, NULL);
        if (!app.settings) {
            warning(L"Windows could not open settings.");
            return;
        }
        for (int i = STARTUP; i < POSITION_SAVED; ++i)
            app.checks[i] = control(L"BUTTON", labels[i - STARTUP], BS_AUTOCHECKBOX | WS_TABSTOP,
                16, 12 + (i - STARTUP) * 23, 506, 22, CHECK_BASE + i);
        app.opacity_label = control(L"STATIC", L"", 0, 16, 246, 506, 20, 0);
        app.opacity_slider = control(TRACKBAR_CLASSW, L"Widget opacity", TBS_NOTICKS | WS_TABSTOP,
            12, 264, 514, 32, 0);
        SendMessageW(app.opacity_slider, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100));
        SendMessageW(app.opacity_slider, TBM_SETPOS, TRUE, app.opacity);
        apply_opacity(app.opacity);
        app.status = control(L"STATIC", L"Ready", 0, 16, 304, 506, 20, 0);
        app.bar = control(PROGRESS_CLASSW, L"", 0, 16, 327, 506, 10, 0);
        app.drives = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            16, 352, 122, 265, DRIVE);
        control(L"BUTTON", L"Rescan", BS_PUSHBUTTON | WS_TABSTOP, 148, 352, 86, 26, SCAN);
        control(L"BUTTON", L"Clean selected", BS_PUSHBUTTON | WS_TABSTOP, 244, 352, 130, 26, CLEAN_SELECTED);
        control(L"BUTTON", L"Clean all drives", BS_PUSHBUTTON | WS_TABSTOP, 384, 352, 138, 26, CLEAN_ALL);
        app.list = control(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
            16, 391, 506, 137, 0);
        sync_checks();
        show_results();
    }
    ShowWindow(app.settings, SW_RESTORE);
    SetForegroundWindow(app.settings);
}

int adk_desktop(uint32_t scope, int minimized, const int32_t *settings) {
    if (!adk_is_elevated()) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    app.instance = GetModuleHandleW(NULL);
    app.icon = LoadIconW(app.instance, MAKEINTRESOURCEW(1));
    app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    memcpy(app.config, settings, sizeof(app.config));
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW widget = {0};
    widget.lpfnWndProc = widget_proc;
    widget.hInstance = app.instance;
    widget.hIcon = app.icon;
    widget.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    widget.lpszClassName = L"ADKRecycleWidget";
    WNDCLASSW settings_window = widget;
    settings_window.lpfnWndProc = settings_proc;
    settings_window.lpszClassName = L"ADKRecycleSettings";
    settings_window.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&widget) || !RegisterClassW(&settings_window)) return 1;
    app.selected = -1;
    update_drives();
    if (scope & app.mask) {
        for (int i = 0; i < 26; ++i) if (scope & (1u << i)) app.selected = i;
    }
    RECT frame = configured_bounds();
    reveal_selection();
    app.widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, widget.lpszClassName,
        L"Advance Recycle Bin Cleaner", WS_POPUP, frame.left, frame.top,
        frame.right - frame.left, frame.bottom - frame.top, NULL, NULL, app.instance, NULL);
    if (!app.widget) return 1;
    // Explorer may have a lower integrity level than the widget.
    ChangeWindowMessageFilterEx(app.widget, TRAY_MESSAGE, MSGFLT_ALLOW, NULL);
    if (app.taskbar_created)
        ChangeWindowMessageFilterEx(app.widget, app.taskbar_created, MSGFLT_ALLOW, NULL);
    app.config[OPACITY] = clamp(app.config[OPACITY], 20, 100);
    if (!apply_opacity(app.config[OPACITY])) {
        DestroyWindow(app.widget);
        return 1;
    }
    BOOL has_tray = add_tray();
    apply_widget_style();
    if (!has_tray || (!minimized && !app.config[HIDE])) ShowWindow(app.widget, SW_SHOWNOACTIVATE);
    if (app.config[AUTO_SCAN] && app.mask) begin(FALSE, -1);
    MSG message;
    BOOL result;
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (app.settings && IsWindowVisible(app.settings) && IsDialogMessageW(app.settings, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (app.worker) {
        WaitForSingleObject(app.worker, INFINITE);
        CloseHandle(app.worker);
        app.worker = NULL;
    }
    if (IsWindow(app.widget)) DestroyWindow(app.widget);
    return result < 0;
}
