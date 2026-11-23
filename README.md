# Advance Recycle Bin Cleaner

Advance Recycle Bin Cleaner is a native Windows utility with a transparent
desktop widget, a settings window, a notification area icon, and
command-line scan and cleanup modes. It uses Zig 0.16.0 and Win32.

## Install on Windows

Build the setup executable using the instructions below, then run
`zig-out/installer/ARBC-Setup-<file-version>-x64.exe` on 64-bit Windows.
Setup installs the widget and CLI for the current user, adds Start menu
shortcuts, and offers an optional desktop shortcut. Quit the widget from
the tray before upgrading or uninstalling. The uninstaller is available in
Windows **Installed apps** and the Start menu. Settings and logs are preserved.

If moving from a portable copy with a JSON file beside the executable, copy
that file into the install folder before launching the installed app. An
existing per-user JSON file takes precedence automatically.

## Build

From the repository root on Windows:

~~~powershell
zig build
zig build test
~~~

Launch `zig-out/bin/ARBC.exe` for the desktop widget. It runs without a
console window and requests administrator approval at launch when needed.
This also applies at sign-in if **Run on Windows startup** is enabled.
Use `advance_recycle_bin_cleaner_cli.exe` for terminal commands and scripts
that wait for an exit code. Both executables share settings when installed
in the same folder.

Build the x64 installer from the repository root:

~~~powershell
.\installer\build.ps1
~~~

The script stages a ReleaseSafe build and writes the installer to
`zig-out/installer/`. See [development and packaging](docs/development.md)
for compiler selection, version metadata, and verification.

## Desktop use

Run the executable to open the widget. It opens flush with the lower-right
corner of the taskbar monitor's work area by default. The widget lists every
detected drive letter and expands upward as drives are added. Short work areas
use a scrollable drive list. Each supported drive shows its recycle-bin size
and file count after a scan.

Click **Clean** on a drive row to clean that drive, or **Clean all** in the
header to clean all supported drives. Cleanup buttons are disabled while an
operation is running. The cleanup mode and confirmation prompt follow Settings.
Click the body of a drive row to select it. Click the same row or the heading
to clear the selection. Drag the widget to save a custom position. Right-click
for scan, cleanup, settings, and close actions. Use **Snap above taskbar** from
the widget or tray menu to return to automatic placement. The tray can also
restore the widget and turn off click-through.

Settings apply immediately and include startup, automatic scanning,
confirmation, cleanup notifications, root-folder deletion, click-through,
widget opacity, and taskbar visibility. The widget's **−** and **×** buttons
follow **Minimize to tray** and **Close to tray**. Enable **Show widget on
taskbar** and turn off **Minimize to tray** to keep the widget on the taskbar
when minimized. The tray's **Quit application** action exits after the active
operation finishes.

## Command line

~~~powershell
.\zig-out\bin\ARBC.exe --minimized
.\zig-out\bin\advance_recycle_bin_cleaner_cli.exe --scan --drive C,D
.\zig-out\bin\ARBC.exe --widget --drive D
.\zig-out\bin\advance_recycle_bin_cleaner_cli.exe --quick-clean --keep-folder --drive D
~~~

**Quick-clean permanently deletes files without prompting.** It targets all
supported drive letters when **--drive** is omitted. **--keep-folder**
overrides the saved folder-deletion setting for that invocation. The default
setting removes the recycle-bin root after its contents are removed. Starting
with **--drive** alone retains the earlier CLI scan behavior. Invalid options,
unavailable selected drive letters, and partial operations return a nonzero
exit status.

## Settings storage

The app first looks for an existing per-user configuration. Otherwise it uses
**advance_recycle_bin_cleaner.json** beside the executable. When that file or
directory is read-only, the app copies the portable settings to
**%LOCALAPPDATA%\AdvanceRecycleBinCleaner\advance_recycle_bin_cleaner.json**.
If the portable file is absent, the app starts from defaults. A malformed or
inaccessible file produces an error and is preserved. Settings updates merge
the fields edited by each app instance and replace the JSON file atomically.

## Scan and cleanup behavior

The widget runs as administrator after startup approval. Cleanups and scans
run in that session, so cleaning does not trigger a second UAC prompt.
**Confirm before cleaning** still controls ARBC's own confirmation dialog.
Canceling startup approval leaves the app closed. CLI scans and unattended
cleanup commands use the permissions of the process that launched them.

The engine opens the recycle bin for mounted local fixed and removable
volumes. It counts accessible filesystem entries, including bin metadata.
Counts can differ from Windows Shell estimates. Reported logical bytes can
differ from the drive's actual free-space change, which is measured separately
before and after cleanup.

Windows permissions, locked files, and unsupported filesystem operations can
leave partial results. Junctions and other reparse entries are skipped and
reported. The root is removed only after its accessible tree is processed
without errors. A cleanup is followed by a fresh scan. Windows notification
settings determine whether a requested completion notification is displayed.
If a Dev Drive uses a dynamic VHDX, reclaiming space inside the volume may
still require compacting the VHDX to reclaim space on its host drive.

## Activity and error logs

Logs are stored in **%LOCALAPPDATA%\AdvanceRecycleBinCleaner\Logs**.

- **ARBC.log** records app startup, scan and cleanup summaries, and errors.
- **ARBC-error.log** records failures, including drive letters and Windows
  error codes. The app logs rotate at 1 MiB and keep one previous copy each.
- **Setup.log** and **Setup-error.log** record installation and uninstall
  events and failures. Setup leaves the logs in place after uninstall.

Operation logs contain aggregate file counts and sizes. If Windows uses a
different administrator account for elevation, the widget's detailed log is
written in that account's Local AppData.

The [development notes](docs/development.md) record the current implementation
and test coverage. The [original plan](plan.md) is kept for design history.
