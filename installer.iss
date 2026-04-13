; Viewfinder installer — Inno Setup 6
; Download compiler: https://jrsoftware.org/isdl.php
; Build: iscc installer.iss

[Setup]
AppId={{A4C2E7F1-3B8D-4E1A-9F5C-2D6B0E3A7C8F}
AppName=Viewfinder
AppVersion=1.0.0
AppPublisher=Viewfinder
AppPublisherURL=
UninstallDisplayName=Viewfinder
UninstallDisplayIcon={app}\viewfinder.exe
DefaultDirName={autopf}\Viewfinder
DefaultGroupName=Viewfinder
DisableProgramGroupPage=yes
SetupIconFile=viewfinder.ico
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=dist
OutputBaseFilename=viewfinder-setup
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; \
    Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; \
    Flags: unchecked

[Files]
Source: "viewfinder.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Viewfinder";       Filename: "{app}\viewfinder.exe"
Name: "{autodesktop}\Viewfinder"; Filename: "{app}\viewfinder.exe"; \
    Tasks: desktopicon

[Run]
Filename: "{app}\viewfinder.exe"; \
    Description: "{cm:LaunchProgram,Viewfinder}"; \
    Flags: nowait postinstall skipifsilent
