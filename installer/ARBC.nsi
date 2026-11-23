Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!ifndef PRODUCT_KEY
    !define PRODUCT_KEY "AdvanceRecycleBinCleaner"
!endif
!ifndef PRODUCT_NAME
    !define PRODUCT_NAME "Advance Recycle Bin Cleaner"
!endif
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_KEY}"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define LOG_DIR "$LOCALAPPDATA\${PRODUCT_KEY}\Logs"

Name "${PRODUCT_NAME}"
OutFile "${SETUP_PATH}"
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT_NAME}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"

!define MUI_ICON "..\assets\icon.ico"
!define MUI_UNICON "..\assets\icon.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\ARBC.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${PRODUCT_NUMERIC}"
VIFileVersion "${FILE_NUMERIC}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "${PRODUCT_NAME} Setup"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "${FILE_VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "CopyLeft - 2026 ADK"
VIAddVersionKey /LANG=${LANG_ENGLISH} "OriginalFilename" "${SETUP_NAME}"

!macro Log File Text
    CreateDirectory "${LOG_DIR}"
    ClearErrors
    FileOpen $R7 "${LOG_DIR}\${File}" a
    ${IfNot} ${Errors}
        FileSeek $R7 0 END
        ${GetTime} "" "L" $R0 $R1 $R2 $R3 $R4 $R5 $R6
        FileWriteUTF16LE /BOM $R7 "$R2-$R1-$R0 $R4:$R5:$R6 ${Text}$\r$\n"
        FileClose $R7
    ${EndIf}
!macroend

!macro CheckClosed Exe Failed
    ${If} ${FileExists} "$INSTDIR\${Exe}"
        System::Call 'kernel32::CreateFileW(w "$INSTDIR\${Exe}", i 0x40000000, i 0, p 0, i 3, i 0x80, p 0) p.r0'
        StrCmp $0 -1 ${Failed}
        System::Call 'kernel32::CloseHandle(p r0)'
    ${EndIf}
!macroend

Function .onInit
    SetShellVarContext current
    !insertmacro Log "Setup.log" "[INFO] Setup ${FILE_VERSION} started."
    ${IfNot} ${RunningX64}
        !insertmacro Log "Setup-error.log" "[ERROR] x64 Windows is required."
        MessageBox MB_OK|MB_ICONSTOP "This installer requires 64-bit Windows." /SD IDOK
        SetErrorLevel 1
        Quit
    ${EndIf}
    SetRegView 64
FunctionEnd

Function .onInstSuccess
    !insertmacro Log "Setup.log" "[INFO] Installation completed in $INSTDIR."
FunctionEnd

Function .onInstFailed
    !insertmacro Log "Setup-error.log" "[ERROR] Installation ended unsuccessfully in $INSTDIR."
FunctionEnd

Section "Advance Recycle Bin Cleaner" MainSection
    SectionIn RO
    !insertmacro Log "Setup.log" "[INFO] Installing in $INSTDIR."
    ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $1 != ""
    ${AndIf} $1 != $INSTDIR
        !insertmacro Log "Setup-error.log" "[ERROR] An installation already exists in $1."
        MessageBox MB_OK|MB_ICONSTOP "Advance Recycle Bin Cleaner is already installed in $1. Select that folder or uninstall it first." /SD IDOK
        Abort
    ${EndIf}
check:
    !insertmacro CheckClosed "ARBC.exe" busy
    !insertmacro CheckClosed "advance_recycle_bin_cleaner_cli.exe" busy
    Goto copyFiles
busy:
    !insertmacro Log "Setup-error.log" "[ERROR] Existing application files are running or unwritable in $INSTDIR."
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Quit Advance Recycle Bin Cleaner from its tray menu and close any cleaner commands, then retry. If it is already closed, check write permissions for the installation folder." /SD IDCANCEL IDRETRY check
    Abort
copyFiles:
    ClearErrors
    SetOutPath "$INSTDIR"
    IfErrors failed
    SetOverwrite on
    File "${PAYLOAD_DIR}\ARBC.exe"
    IfErrors failed
    File "${PAYLOAD_DIR}\advance_recycle_bin_cleaner_cli.exe"
    IfErrors failed
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    IfErrors failed
    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    IfErrors failed
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\ARBC.exe"
    IfErrors failed
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    IfErrors failed
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "ADK"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\ARBC.exe,0"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    IfErrors failed
    Goto done
failed:
    !insertmacro Log "Setup-error.log" "[ERROR] A file, shortcut, or registry entry could not be written."
    Abort
done:
SectionEnd

Section /o "Desktop shortcut" DesktopSection
    ClearErrors
    CreateShortcut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\ARBC.exe"
    ${If} ${Errors}
        !insertmacro Log "Setup-error.log" "[ERROR] Could not create the optional desktop shortcut."
    ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${MainSection} "Install the widget, command-line companion, and Start menu shortcuts."
    !insertmacro MUI_DESCRIPTION_TEXT ${DesktopSection} "Also add a shortcut to your desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
    SetShellVarContext current
    SetRegView 64
    !insertmacro Log "Setup.log" "[INFO] Uninstall started in $INSTDIR."
FunctionEnd

Function un.onUninstFailed
    !insertmacro Log "Setup-error.log" "[ERROR] Uninstall ended unsuccessfully in $INSTDIR."
FunctionEnd

Section "Uninstall"
check:
    !insertmacro CheckClosed "ARBC.exe" busy
    !insertmacro CheckClosed "advance_recycle_bin_cleaner_cli.exe" busy
    Goto removeFiles
busy:
    !insertmacro Log "Setup-error.log" "[ERROR] Application files are running or unwritable in $INSTDIR."
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Quit Advance Recycle Bin Cleaner from its tray menu and close any cleaner commands, then retry." /SD IDCANCEL IDRETRY check
    Abort
removeFiles:
    ClearErrors
    Delete "$INSTDIR\ARBC.exe"
    Delete "$INSTDIR\advance_recycle_bin_cleaner_cli.exe"
    ${If} ${Errors}
        !insertmacro Log "Setup-error.log" "[ERROR] Could not remove one or more application files."
        Abort
    ${EndIf}
    ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $1 == $INSTDIR
        Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
        Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"
        Delete "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk"
        RMDir "$SMPROGRAMS\${PRODUCT_NAME}"
        DeleteRegKey HKCU "${UNINSTALL_KEY}"
    ${EndIf}
    ReadRegStr $1 HKCU "${RUN_KEY}" "${PRODUCT_KEY}"
    ${If} $1 == '"$INSTDIR\ARBC.exe" --minimized'
        DeleteRegValue HKCU "${RUN_KEY}" "${PRODUCT_KEY}"
    ${EndIf}
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    !insertmacro Log "Setup.log" "[INFO] Uninstall completed. Settings and logs were retained."
SectionEnd
