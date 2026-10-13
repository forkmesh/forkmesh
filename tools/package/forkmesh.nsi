; NSIS installer template for the ForkMesh desktop client (issue #370).
; Driven by tools/package/windows.sh, which passes:
;   /DFORKMESH_VERSION=X.Y.Z  /DFORKMESH_PAYLOAD=<deployed app dir>
;   /DFORKMESH_OUTFILE=<setup exe>
; See /docs#installers-updates.

!ifndef FORKMESH_VERSION
  !define FORKMESH_VERSION "0.0.0"
!endif
!ifndef FORKMESH_PAYLOAD
  !error "FORKMESH_PAYLOAD (windeployqt output directory) must be defined"
!endif
!ifndef FORKMESH_UNINSTALL_MANIFEST
  !error "FORKMESH_UNINSTALL_MANIFEST (tools/package/nsis-uninstall-manifest.sh output) must be defined"
!endif
!ifndef FORKMESH_OUTFILE
  !define FORKMESH_OUTFILE "forkmesh-windows-setup.exe"
!endif

!include "MUI2.nsh"

Name "ForkMesh ${FORKMESH_VERSION}"
OutFile "${FORKMESH_OUTFILE}"
Unicode true
; Per-user install: no admin elevation, no UAC prompt — matches the download-and-run
; expectation and lets a newer setup.exe upgrade in place without elevation.
InstallDir "$LOCALAPPDATA\ForkMesh"
RequestExecutionLevel user
SetCompressor /SOLID lzma

VIProductVersion "${FORKMESH_VERSION}.0"
VIAddVersionKey "ProductName"     "ForkMesh"
VIAddVersionKey "FileDescription" "ForkMesh desktop client"
VIAddVersionKey "FileVersion"     "${FORKMESH_VERSION}"
VIAddVersionKey "ProductVersion"  "${FORKMESH_VERSION}"
VIAddVersionKey "CompanyName"     "ForkMesh"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ForkMesh"

Section "ForkMesh" SecMain
  SetOutPath "$INSTDIR"
  File /r "${FORKMESH_PAYLOAD}\*"

  CreateShortCut "$SMPROGRAMS\ForkMesh.lnk" "$INSTDIR\forkmesh.exe"
  CreateShortCut "$DESKTOP\ForkMesh.lnk" "$INSTDIR\forkmesh.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr   HKCU "${REGKEY}" "DisplayName"     "ForkMesh"
  WriteRegStr   HKCU "${REGKEY}" "DisplayVersion"  "${FORKMESH_VERSION}"
  WriteRegStr   HKCU "${REGKEY}" "Publisher"       "ForkMesh"
  WriteRegStr   HKCU "${REGKEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr   HKCU "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1
  WriteRegStr HKCU "Software\Classes\forkmesh" "" "URL:ForkMesh local control link"
  WriteRegStr HKCU "Software\Classes\forkmesh" "URL Protocol" ""
  WriteRegStr HKCU "Software\Classes\forkmesh\DefaultIcon" "" "$INSTDIR\forkmesh.exe,0"
  WriteRegStr HKCU "Software\Classes\forkmesh\shell\open\command" "" "$\"$INSTDIR\forkmesh.exe$\" $\"%1$\""
  WriteRegStr HKCU "Software\Classes\Directory\shell\ForkMesh" "" "Open with ForkMesh"
  WriteRegStr HKCU "Software\Classes\Directory\shell\ForkMesh" "Icon" "$INSTDIR\forkmesh.exe,0"
  WriteRegStr HKCU "Software\Classes\Directory\shell\ForkMesh\command" "" "$\"$INSTDIR\forkmesh.exe$\" $\"%1$\""
  WriteRegStr HKCU "Software\Classes\Directory\Background\shell\ForkMesh" "" "Open with ForkMesh"
  WriteRegStr HKCU "Software\Classes\Directory\Background\shell\ForkMesh" "Icon" "$INSTDIR\forkmesh.exe,0"
  WriteRegStr HKCU "Software\Classes\Directory\Background\shell\ForkMesh\command" "" "$\"$INSTDIR\forkmesh.exe$\" $\"%V$\""
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\ForkMesh.lnk"
  Delete "$DESKTOP\ForkMesh.lnk"
  ; Never `RMDir /r "$INSTDIR"`: the default $INSTDIR, %LOCALAPPDATA%\ForkMesh,
  ; also holds %LOCALAPPDATA%\ForkMesh\ForkMesh -- the legacy local data root
  ; (identity, settings, caches). Remove exactly the files this installer
  ; copied, then each payload folder only if it is now empty.
  !include "${FORKMESH_UNINSTALL_MANIFEST}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  ; Builds up to v1.2.3 put the mirror Actions transaction lock under
  ; <drive>:\HKEY_CURRENT_USER\Software\ForkMesh (a registry key name read as
  ; a path; issue #727). Remove only those exact lock/journal names, then the
  ; three folders if -- and only if -- they are empty. Never recursive.
  StrCpy $0 "$INSTDIR" 3
  Delete "$0HKEY_CURRENT_USER\Software\ForkMesh\.forkmesh-actions-configuration-*.lock"
  Delete "$0HKEY_CURRENT_USER\Software\ForkMesh\.forkmesh-actions-configuration-*.json"
  RMDir "$0HKEY_CURRENT_USER\Software\ForkMesh"
  RMDir "$0HKEY_CURRENT_USER\Software"
  RMDir "$0HKEY_CURRENT_USER"
  DeleteRegKey HKCU "${REGKEY}"
  DeleteRegKey HKCU "Software\Classes\forkmesh"
  DeleteRegKey HKCU "Software\Classes\Directory\shell\ForkMesh"
  DeleteRegKey HKCU "Software\Classes\Directory\Background\shell\ForkMesh"
SectionEnd
