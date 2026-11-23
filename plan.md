# Advance Recycle Bin Cleaner — Full Detailed Implementation Plan (Zig)

> This is the original design plan. See the [README](README.md) for current
> behavior and the [development notes](docs/development.md) for implementation
> and verification details.

## 1. Product Vision & Design Goals

**Advance Recycle Bin Cleaner** is a minimalist, compact, high-performance Windows desktop utility written in **Zig**. It combines a `250px x 65px` transparent Rainmeter-style desktop widget, a standard Windows GUI Settings window, a system tray controller, and a CLI quick-clean mode for taskbar pinning.

### Core Design Principles

* **Zero External Runtime Dependencies:** Compiles into a single standalone native `.exe` using Zig's standard library and direct Windows API bindings (`kernel32`, `user32`, `gdi32`, `shell32`, `advapi32`).
* **Minimalist & Compact Footprint:** Low idle CPU and memory usage; flat, clean visual hierarchy; compact `250px x 65px` primary desktop widget.
* **Immediate Auto-Persistence:** Every setting change, window/widget position change, or tray mode toggle auto-loads on startup and auto-saves immediately to disk.
* **Non-Blocking Execution & Visual Feedback:** Long-running drive scans and cleanup operations run on worker threads (`std.Thread`) while feeding live progress (`0.0`–`1.0`) and smooth UI animations back to the UI thread.

---

## 2. System Architecture & Surfaces

### 2.1 `250px x 65px` Transparent Desktop Widget

* **Dimensions & Chrome:** Exact `250px` width by `65px` height frameless layered window (`WS_POPUP | WS_EX_LAYERED`), supporting transparent color-keying / alpha compositing so only the compact widget card and text/progress elements render visibly.
* **Data Readouts:**
* Header line: `$RECYCLE.BIN` and current view scope (`ALL` or specific drive letter, e.g., `C:\`, `D:\`, Dev Drive).
* Primary metric: Reclaimable size / estimated space gained back (`+X.XX GB` or `0 B`) and current state (`Scanning...`, `Cleaning...`, `Idle`).
* Subline: File count, bin status, or free space transition (`Free: before -> after`).


* **Inline Progress Bar & Animations:**
* Compact progress bar integrated along the bottom edge of the widget during scanning or cleaning.
* Smooth interpolated transitions (`lerp`) driven by a lightweight UI animation timer (`SetTimer`), active during scans/cleanups/state transitions and automatically sleeping/idling when stationary so CPU stays near zero.


* **Interactions:**
* **Left-click (without drag):** Cycles between `ALL` drives and each individual detected drive view.
* **Left-click + drag:** Moves the widget across monitors/desktop and auto-saves `(x, y)` coordinates on release (when click-through is disabled).
* **Right-click:** Opens a compact context menu with:
* `Rescan Drives`
* `Clean Selected View`
* `Clean All Drives`
* `Hide to Tray`
* `Toggle Click-Through`
* `Settings...`
* `Exit` (honors `exit_to_tray` setting)





### 2.2 Normal GUI Settings Window

* **Window Style:** Standard native Windows GUI window (`WS_OVERLAPPEDWINDOW` or standard styled top-level window with title bar, minimize button, and close button) so it behaves like a normal desktop configuration window.
* **Configuration Toggles (All Auto-Save on Change):**
* **Hide to Tray:** Hides the `250x65` widget and/or active UI surfaces to the notification area while keeping the background tray icon alive.
* **Minimize to Tray:** Intercepts window minimize events (`WM_SIZE` with `SIZE_MINIMIZED` / `SC_MINIMIZE`) on the Settings/GUI window to hide to the system tray instead of leaving a minimized taskbar button.
* **Exit to Tray:** Intercepts close/exit requests (`WM_CLOSE` / `SC_CLOSE`) so closing the window tucks the app into the system tray rather than terminating the process; provides an explicit **Quit App** action in the tray menu to terminate cleanly.
* **Run on Windows Startup:** Adds or removes the executable/launch command under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
* **Auto-Scan on Launch:** Automatically scans all logical drives upon startup when enabled.
* **Confirm Before Cleaning:** Toggles confirmation prompts before destructive clean/delete actions.
* **Show Cleanup Notifications:** Enables or suppresses tray balloon/toast notifications after scans/cleanups.
* **Delete `$Recycle.Bin` Folder:** Toggles between deleting the `$Recycle.Bin` folder tree itself (recreated by Windows as needed) vs. emptying only its contents while keeping the root directory.
* **Click-Through Overlay Mode:** Toggles `WS_EX_TRANSPARENT` on the `250x65` layered widget so mouse clicks pass through to underlying windows.


* **Progress Bar & Activity Section:**
* Displays a dedicated progress bar, current target drive/step label, animated progress indicator, and per-drive scan/gain-back summary with quick **Rescan**, **Clean Selected**, and **Clean All** actions.



### 2.3 System Tray & Notifications

* Managed via `Shell_NotifyIconW` with a custom `WM_APP` callback message.
* Tray menu actions:
* `Show / Restore Widget`
* `Open Settings`
* `Rescan Drives`
* `Clean All Drives`
* `Quit Application` (true process exit regardless of `exit_to_tray`)


* Displays balloon/toast notifications after cleanups when `notifications` is enabled.

### 2.4 CLI & Taskbar Quick-Clean Mode

* Supports command-line flags for pinned taskbar shortcuts or scheduled tasks:
* `--quick-clean`: Runs immediate cleanup without requiring manual UI interaction, displays notification if enabled, and exits.
* `--drive <LETTERS>`: Targets specific drive letters (e.g., `--drive C,D` or `--drive D`); defaults to all logical drives.
* `--keep-folder`: Overrides `delete_root_folder` for the run to clean contents only while preserving `$Recycle.Bin`.
* `--minimized`: Starts hidden/tucked to tray rather than showing the widget immediately.



---

## 3. Source File & Module Layout (`src/`)

```text
src/
├── main.zig          # Entrypoint, CLI parsing, config auto-load, lifecycle & message loop
├── config.zig        # Config struct, JSON auto-load/auto-save, Windows Run registry toggle
├── scanner.zig       # Drive enumeration, disk free space, recursive $Recycle.Bin metrics, progress
├── cleaner.zig       # Clean contents vs. delete root folder, permission retry, progress, errors
├── ui_widget.zig     # 250x65 transparent Win32 widget, animations, progress bar, drag & menu
├── ui_settings.zig   # Normal Win32 GUI Settings window, tray toggles, progress bar & drive list
└── tray_notify.zig   # Shell_NotifyIconW tray icon, tray context menu & notifications

```

### Detailed Module Responsibilities

1. **`src/config.zig`**
* Defines `Config` with safe defaults (`x = 120`, `y = 120`, `run_on_startup = false`, `auto_scan = true`, `confirm_delete = true`, `notifications = true`, `delete_root_folder = true`, `click_through = false`, `hide_to_tray = false`, `minimize_to_tray = true`, `exit_to_tray = true`).
* `loadOrCreate(allocator)`: Reads `advance_recycle_bin_cleaner.json` next to the executable; creates default file if missing; falls back cleanly to defaults on parse errors.
* `save(cfg)`: Serializes and writes config atomically/safely immediately after any state mutation (toggle flip, click-through toggle, or widget drag end).
* `syncStartupRegistry(enabled)`: Updates `Software\Microsoft\Windows\CurrentVersion\Run` under `HKEY_CURRENT_USER`.


2. **`src/scanner.zig`**
* Uses `GetLogicalDrives` to discover active drive letters (`A:`–`Z:`), including fixed drives, removable/external drives, and Dev Drives.
* Uses `GetDiskFreeSpaceExW` to query `free_bytes_available_to_caller` (`free_before`) for each drive.
* Recursively traverses `<Drive>:\$Recycle.Bin` to compute:
* `exists: bool`
* `bin_bytes: u64`
* `file_count: u64`
* `gain_back_bytes()` (`bin_bytes`)
* `free_after_est()` (`free_before + bin_bytes`)


* Emits progress updates (`processed_drives / total_drives` and current drive) via a thread-safe callback or shared atomic/mutex progress struct + `WM_APP` notification.


3. **`src/cleaner.zig`**
* Supports two modes per drive:
* **Clean Contents (`delete_root = false`):** Iterates entries inside `<Drive>:\$Recycle.Bin` and deletes files and SID subdirectories while keeping `$Recycle.Bin` intact.
* **Delete Root Folder (`delete_root = true`):** Deletes `<Drive>:\$Recycle.Bin` and its entire subtree so Windows recreates it automatically on subsequent deletions.


* Handles read-only attributes (`FILE_ATTRIBUTE_READONLY`) by clearing the attribute and retrying deletion.
* Tracks per-drive progress (`0.0` to `1.0`), estimated bytes reclaimed, and non-fatal errors (e.g., locked files or restricted SID directories) without aborting remaining drives.


4. **`src/ui_widget.zig`**
* Registers and creates the `250px x 65px` transparent layered window.
* Implements animation state:
* Smoothly interpolates `displayed_progress` toward `target_progress` (`0.0`–`1.0`).
* Animates subtle pulse/activity accent during active scan/clean operations.
* Smoothly fades or transitions status/metric highlights on scan completion.


* Draws the compact card, view mode badge (`ALL` or drive letter), reclaimable size, subline details, and bottom progress bar.
* Handles left-click cycling, drag movement + instant config auto-save, click-through style updates (`WS_EX_TRANSPARENT`), and right-click menu dispatch.


5. **`src/ui_settings.zig`**
* Implements a standard native GUI Settings window (`WS_OVERLAPPEDWINDOW` without resizing clutter or with clean fixed/clamped bounds).
* Renders checkboxes/toggles for:
* `Hide to Tray`
* `Minimize to Tray`
* `Exit to Tray`
* `Run on Windows Startup`
* `Auto-Scan on Launch`
* `Confirm Before Cleaning`
* `Show Cleanup Notifications`
* `Delete $Recycle.Bin Folder`
* `Click-Through Widget Mode`


* Every toggle immediately mutates `Config`, calls `config.save(...)`, updates live window/widget/tray/registry state, and reflects status clearly.
* Includes a live progress bar, current operation status text, drive breakdown table/list, and `Rescan`, `Clean Selected`, and `Clean All` controls.


6. **`src/tray_notify.zig`**
* Creates and updates the `NOTIFYICONDATAW` entry via `Shell_NotifyIconW` (`NIM_ADD`, `NIM_MODIFY`, `NIM_DELETE`).
* Handles tray icon double-click/left-click (restore widget/settings) and right-click popup menu (`Show Widget`, `Settings`, `Clean All`, `Quit App`).
* Sends notifications (`NIF_INFO`) when cleanup completes and `notifications` is enabled.


7. **`src/main.zig`**
* Initializes allocator (`GeneralPurposeAllocator` in debug, `smp` / `GPA` in release), loads config, parses CLI args, and starts either headless `--quick-clean` or the GUI/tray message loop.
* Coordinates worker threads and `WM_APP_*` messages across `ui_widget.zig`, `ui_settings.zig`, and `tray_notify.zig`.



---

## 4. Data Flows & State Transitions

### 4.1 Auto-Load & Auto-Save Flow

1. On startup, `config.loadOrCreate` reads `advance_recycle_bin_cleaner.json` (creating defaults if absent).
2. `main.zig` applies initial window/widget position `(x, y)`, `click_through`, and `run_on_startup`.
3. Whenever a user toggles `hide_to_tray`, `minimize_to_tray`, `exit_to_tray`, or any other option in the Settings window or context menu—or finishes dragging the `250x65` widget—`config.save` writes the updated JSON immediately.

### 4.2 Tray Behavior Rules

* **Hide to Tray:** Hides the `250x65` widget (`ShowWindow(widget_hwnd, SW_HIDE)`) while keeping the tray icon active so the user can restore it anytime from the tray menu.
* **Minimize to Tray:** When the Settings GUI window receives a minimize event and `minimize_to_tray == true`, hides the window (`SW_HIDE`) instead of minimizing to the taskbar.
* **Exit to Tray:** When the Settings GUI window or widget receives a close/exit action and `exit_to_tray == true`, hides the window(s) into the tray rather than calling `PostQuitMessage(0)`. Selecting **Quit App** from the tray menu always performs a full cleanup (`NIM_DELETE`, `KillTimer`, join threads) and exits cleanly.

### 4.3 Scan, Clean, Progress & Animation Flow

1. User triggers Scan (on launch if `auto_scan` is true, via `Rescan`, or automatically after cleanup).
2. Worker thread iterates drives, updating `progress` (`0.0` -> `1.0`) and posting `WM_APP_SCAN_PROGRESS` and `WM_APP_SCAN_DONE`.
3. UI timer (`~16 ms` interval while animating) interpolates progress bars and activity indicators smoothly in both the `250x65` widget and the Settings window, stopping timer work when settled at idle.
4. User triggers Clean/Delete (per-drive or all drives); optional confirmation dialog runs on UI thread if `confirm_delete == true`; worker thread executes deletion with progress callbacks, posts completion, triggers notification if enabled, and runs a follow-up scan.

---

## 5. Windows, Dev Drive & Permission Edge Cases

* **Dev Drives & Dynamic VHDX:** Removing items from `<DevDrive>:\$Recycle.Bin` frees space inside the Dev Drive volume. If the Dev Drive is hosted on a dynamically expanding `.vhdx` file, compacting the `.vhdx` may still be required to reclaim space on the underlying host drive.
* **Restricted SID Folders & Locked Files:** `$Recycle.Bin` contains per-user SID subdirectories (`S-1-5-21-...`). Files owned by another user or currently locked by a process may return `ERROR_ACCESS_DENIED` or `ERROR_SHARING_VIOLATION`; the cleaner should log/display partial warnings per drive and note that running as Administrator can help for restricted SID entries.
* **Click-Through Safety:** When `click_through` is enabled on the `250x65` widget, left-click/drag on the widget passes through to windows underneath; users can disable click-through from the Settings window or tray menu.

---

## 6. Phased Build & Verification Plan

### Phase 1: Config Auto-Save/Auto-Load & Core Scanner/Cleaner

* Implement `config.zig`, `scanner.zig`, and `cleaner.zig` with unit/integration tests on temporary directories and logical drives.
* **Verification:** Confirm config auto-creates, auto-loads, and auto-saves on every change; confirm accurate byte/file counts, `free_before -> free_after_est` calculations, clean vs. folder-delete behavior, and read-only attribute retry.

### Phase 2: `250x65` Transparent Widget, Progress Bar & Animations

* Implement `ui_widget.zig` with exact `250x65` dimensions, transparent background, click cycling, drag-and-save coordinates, right-click menu, progress bar, and smooth animation interpolation.
* **Verification:** Confirm `250x65` layout, transparency around card edges, smooth progress/animation updates during scans/cleanups, zero drift on drag coordinate persistence, and proper click-through toggling.

### Phase 3: Normal GUI Settings Window, Tray Toggles & CLI Quick-Clean

* Implement `ui_settings.zig`, `tray_notify.zig`, and `main.zig` CLI flags (`--quick-clean`, `--drive`, `--keep-folder`, `--minimized`).
* **Verification:** Confirm standard Settings window behavior, **Hide to Tray**, **Minimize to Tray**, **Exit to Tray**, tray restore/quit, post-clean notifications, and `--quick-clean` execution from a pinned taskbar shortcut.
