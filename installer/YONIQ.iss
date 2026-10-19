#define MyAppName "YONIQ"
#define MyAppVersion "1.13.0"
#define MyAppURL "https://github.com/kc8tyk2393/YONIQ"
#define MyAppExeName "Yoniq.exe"

[Setup]
AppId={{87EB2E2E-D7A8-4EE3-A7AB-81A81F96EC4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName=C:\Ham\MMSSTV
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=stage\Terms.txt
OutputDir=..\dist
OutputBaseFilename=YONIQ-setup
SetupIconFile=stage\mmsstv_i.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x86compatible
PrivilegesRequired=admin
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "stage\*"; DestDir: "{app}"; Excludes: "Config.cfg"; Flags: ignoreversion
Source: "stage\Config.cfg"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "stage\Stock\*"; DestDir: "{app}\Stock"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\mmsstv_i.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\mmsstv_i.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
