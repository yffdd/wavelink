#ifndef MyAppVersion
  #define MyAppVersion "0.0"
#endif

[Setup]
AppId={{9F5B2F99-4A73-4D95-8F32-8D13F2716E34}
AppName=WaveLink
AppVersion={#MyAppVersion}
AppPublisher=xiales
AppPublisherURL=https://xiales.lanzouu.com/b007unma2b
AppSupportURL=https://xiales.lanzouu.com/b007unma2b
AppUpdatesURL=https://xiales.lanzouu.com/b007unma2b
DefaultDirName={autopf}\WaveLink
DisableProgramGroupPage=yes
; Output to the setup directory
OutputDir=.
OutputBaseFilename=WaveLink_v{#MyAppVersion}_Windows_x64_Setup
SetupIconFile=..\..\WaveLink\resources\app.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "WaveLink_Release\WaveLink.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "WaveLink_Release\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{app}\卸载 WaveLink"; Filename: "{uninstallexe}"
Name: "{autoprograms}\WaveLink"; Filename: "{app}\WaveLink.exe"
Name: "{autodesktop}\WaveLink"; Filename: "{app}\WaveLink.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\WaveLink.exe"; Description: "{cm:LaunchProgram,WaveLink}"; Flags: nowait postinstall skipifsilent
