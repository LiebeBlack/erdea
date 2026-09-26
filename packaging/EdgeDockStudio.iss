; ============================================================================
;  EdgeDock Studio - script de instalador para Inno Setup 6
;
;  Compilacion completa (el ejecutable y el icono deben existir antes):
;    cmake -S . -B build -A x64
;    cmake --build build --config Release --parallel
;    powershell -ExecutionPolicy Bypass -File packaging/make-icon.ps1 -OutFile build/EdgeDock.ico
;    ISCC.exe /DAppVersion=1.0.0 packaging/EdgeDockStudio.iss
;
;  Salida: dist/installer/EdgeDockStudio-Setup-<version>.exe
;
;  Decisiones de diseno:
;   - Instalacion por maquina en {autopf} (Program Files de 64 bits).
;   - Autoarranque opcional via HKCU (el panel mismo tambien lo permite).
;   - %APPDATA%\EdgeDock (clips.db, notas, config) NUNCA se elimina al desinstalar.
; ============================================================================

#ifndef AppVersion
#define AppVersion "0.0.0"
#endif

#define AppName "EdgeDock Studio"
#define AppExe "EdgeDockStudio.exe"
#define RepoRoot ".."
#define BuildDir RepoRoot + "\build\Release"
#define IconFile RepoRoot + "\build\EdgeDock.ico"

#if !FileExists(BuildDir + "\" + AppExe)
#pragma error "Falta build\Release\EdgeDockStudio.exe - compila antes en Release: cmake --build build --config Release"
#endif

#if !FileExists(IconFile)
#pragma error "Falta build\EdgeDock.ico - generalo con: powershell -ExecutionPolicy Bypass -File packaging/make-icon.ps1 -OutFile build/EdgeDock.ico"
#endif

[Setup]
AppId={{F3A8C7D2-6B1E-4A9F-8C5D-2E0B7A4D9C61}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=EdgeDock Studio Project
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir={#RepoRoot}\dist\installer
OutputBaseFilename=EdgeDockStudio-Setup-{#AppVersion}
SetupIconFile={#IconFile}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
CloseApplications=yes

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Iniciar {#AppName} al arrancar Windows"; GroupDescription: "Opciones"; Flags: unchecked

[Files]
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#IconFile}"; DestDir: "{app}"; DestName: "EdgeDock.ico"; Flags: ignoreversion
Source: "{#RepoRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\EdgeDock.ico"; IconIndex: 0
Name: "{group}\Desinstalar {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\EdgeDock.ico"; IconIndex: 0; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "EdgeDockStudio"; ValueData: """{app}\{#AppExe}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

; Sin [UninstallDelete]: los datos de usuario en %APPDATA%\EdgeDock se conservan.
