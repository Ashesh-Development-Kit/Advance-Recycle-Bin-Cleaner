# Development and packaging

ARBC targets Windows. Build with Zig 0.16.0 from the repository root.

~~~powershell
zig build
zig build test --summary all
.\installer\build.ps1
~~~

The normal build installs ARBC.exe and advance_recycle_bin_cleaner_cli.exe
under zig-out/bin. The setup script stages a separate x64 ReleaseSafe build
under .zig-cache and writes the per-user installer to zig-out/installer.
It uses MakeNSIS on PATH or downloads a checksum-verified portable NSIS 3.12
compiler into the build cache. Pass -MakeNsis with a compiler path to use a
specific installed copy.

The installer name and version fields are taken from the compiled desktop
executable. Update both the numeric values and display strings in
[app.rc](../app.rc) when changing versions. For this build, the desktop file
version is **0.5.2.8-eab** and the product version is **0.3.1.8-m**. The
desktop resource uses English (Australia) and records ARBC.exe as the original
filename. The console companion records its own filename.

Generated executables, symbols, installer tooling, user settings, and logs are
excluded from Git. The source icon and its Windows ICO are tracked in
[assets](../assets).

## Source layout

| Area | Responsibility |
| --- | --- |
| [main.zig](../src/main.zig) and [root.zig](../src/root.zig) | CLI options, configuration, and the desktop launch handoff |
| [desktop.c](../src/desktop.c) | Widget, tray, settings, administrator launch, and background work |
| [engine.c](../src/engine.c) | Local volume discovery, scans, and both recycle-bin cleanup modes |
| [config.zig](../src/config.zig) and [log.c](../src/log.c) | Configuration storage, atomic saves, and rotating activity and error logs |
| [installer](../installer) | Setup build script, shortcuts, uninstall registration, and setup logs |

The desktop starts through the asInvoker manifest. A launch from a regular
Windows session requests elevation and passes the widget options to the
elevated instance. The widget only opens after verifying its token. This
allows the existing per-user Run entry to launch the app at sign-in and
request approval once. Cleanup uses a worker thread in the elevated widget.
CLI commands use the token of the calling process.

The engine resolves drive letters to mounted local volumes and opens entries
relative to parent directory handles. It reports partial failures and skips
reparse points. The installer removes the files and shortcuts it installed
and preserves portable or per-user settings and logs.

## Verification

- The automated suite covers argument parsing, JSON defaults, configuration
  merging and malformed files, startup registration in an isolated registry
  key, and cleanup inside temporary directory fixtures. The cleanup fixture
  covers read-only and locked files and checks that a junction's destination
  stays intact.
- Windows smoke testing verified a read-only scan of a real drive, both
  cleanup modes on disposable NTFS volumes, and cleanup from an already
  elevated widget without a second elevation request. The interactive UAC
  approval and cancellation paths were not exercised.
- Widget testing covered taskbar placement and visibility, resize on volume
  arrival and removal, a saved position on a second monitor, opacity, tray
  recovery, and exit while a scan finishes. Live screen samples confirmed
  stable drive rows during progress animation.
- Installer testing covered fresh install, reinstall, Start menu shortcuts,
  uninstall registration, the locked-executable error path, and preservation
  of settings and logs after uninstall. Log testing covered Windows error
  codes and app log rotation.

Use temporary directory fixtures or a disposable volume for destructive
verification. Notification banner delivery depends on Windows settings and
remains a manual check.

See the [README](../README.md) for user-facing instructions.
