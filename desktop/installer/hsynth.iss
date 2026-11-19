; HSynth for Windows - Inno Setup script. Built by desktop/tools/package_windows.py, which passes
; /DAppVersion=x.y.z and /DStage=<folder with hsynth_desktop.exe, factory\, README.txt>.
; Per-user install (no administrator rights): %LOCALAPPDATA%\Programs\HSynth. The user's data
; (presets, samples, setups, MIDI files) goes to Documents\HSynth and stays when uninstalling.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\..\dist\stage"
#endif

[Setup]
AppId={{6E3B8F2A-41C7-4C2B-9B5E-2F4D1A7C9E10}
AppName=HSynth
AppVersion={#AppVersion}
AppVerName=HSynth {#AppVersion}
AppPublisher=tera-works
DefaultDirName={localappdata}\Programs\HSynth
DefaultGroupName=HSynth
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=HSynth-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\hsynth_desktop.exe
UninstallDisplayName=HSynth {#AppVersion}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Dirs]
Name: "{userdocs}\HSynth"; Flags: uninsneveruninstall

[Files]
Source: "{#Stage}\hsynth_desktop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#Stage}\factory\*"; DestDir: "{app}\factory"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\HSynth"; Filename: "{app}\hsynth_desktop.exe"; Parameters: "--data ""{userdocs}\HSynth"" --factory ""{app}\factory"""; WorkingDir: "{app}"
Name: "{group}\HSynth のデータフォルダー"; Filename: "{userdocs}\HSynth"
Name: "{group}\{cm:UninstallProgram,HSynth}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\HSynth"; Filename: "{app}\hsynth_desktop.exe"; Parameters: "--data ""{userdocs}\HSynth"" --factory ""{app}\factory"""; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\hsynth_desktop.exe"; Parameters: "--data ""{userdocs}\HSynth"" --factory ""{app}\factory"""; Description: "{cm:LaunchProgram,HSynth}"; Flags: nowait postinstall skipifsilent
