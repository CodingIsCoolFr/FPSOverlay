; FPS Overlay installer. Needs Inno Setup 6.7 or newer; build.bat compiles it after packaging.
;
;   ISCC [/DAppFiles=<folder with FPSOverlay.exe>] [/DOutDir=<folder>] installer\FPSOverlay.iss
;
; It installs for the current user only, into %LOCALAPPDATA%\Programs\FPS Overlay, so Setup
; itself never asks for admin rights. That folder is writable, so the app keeps its settings
; next to the exe and its automatic updates replace the files in place, exactly as in the zip.

#if VER < EncodeVer(6, 7, 0)
  #error Inno Setup 6.7 or newer is needed (dark wizard, PNG pictures, ExecAndCaptureOutput).
#endif
#ifndef AppFiles
  #define AppFiles AddBackslash(SourcePath) + "..\dist\FPSOverlay"
#endif
#ifndef OutDir
  #define OutDir AddBackslash(SourcePath) + "..\dist"
#endif
#define AppExe "FPSOverlay.exe"
#define AppVersion GetStringFileInfo(AppFiles + "\" + AppExe, "ProductVersion")
#if AppVersion == ""
  #error FPSOverlay.exe was not found in AppFiles. Run build.bat first.
#endif
#define AppRepo "https://github.com/CodingIsCoolFr/FPSOverlay"
; The app's "Start with Windows" task (src/platform/shell.cpp).
#define StartupTask "FPS Overlay"

[Setup]
AppId={{6E0F3C2A-8B1D-4F57-9A3E-2C5D7B41F0A9}
AppName=FPS Overlay
AppVersion={#AppVersion}
AppPublisher=CodingIsCoolFr
AppPublisherURL={#AppRepo}
AppSupportURL={#AppRepo}/issues
AppUpdatesURL={#AppRepo}/releases
VersionInfoVersion={#AppVersion}
VersionInfoDescription=FPS Overlay Setup
DefaultDirName={localappdata}\Programs\FPS Overlay
DefaultGroupName=FPS Overlay
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
ShowLanguageDialog=no
CloseApplications=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir={#OutDir}
OutputBaseFilename=FPSOverlaySetup
SetupIconFile=..\icon.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName=FPS Overlay
WizardStyle=modern dark
WizardImageFile=images\wizard-100.png,images\wizard-125.png,images\wizard-150.png,images\wizard-175.png,images\wizard-200.png,images\wizard-250.png
WizardSmallImageFile=images\small-100.png,images\small-125.png,images\small-150.png,images\small-175.png,images\small-200.png,images\small-250.png
Compression=lzma2/ultra64
SolidCompression=yes

[Languages]
; Every language the app speaks that Inno Setup ships a translation for.
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ar"; MessagesFile: "compiler:Languages\Arabic.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "nl"; MessagesFile: "compiler:Languages\Dutch.isl"
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "pt"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "tr"; MessagesFile: "compiler:Languages\Turkish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#AppFiles}\*"; DestDir: "{app}"; Excludes: "config.ini,*.log,update,*.old"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\FPS Overlay"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\FPS Overlay"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; shellexec: the app asks for admin rights, which only ShellExecute can show a prompt for.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,FPS Overlay}"; Flags: nowait postinstall skipifsilent shellexec

[UninstallDelete]
; What the app writes next to itself: settings, log, and update leftovers.
Type: files; Name: "{app}\config.ini"
Type: files; Name: "{app}\FPSOverlay.log"
Type: files; Name: "{app}\*.old"
Type: filesandordirs; Name: "{app}\update"

[Code]
// One click: Setup goes straight to installing. The desktop shortcut task stays on, and
// /MERGETASKS="!desktopicon" still turns it off from the command line.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := PageID = wpSelectTasks;
end;

// The running app holds its exe open, so Setup could not replace it. The app runs as
// administrator, so Setup cannot close it either: ask the user to.
function AppRunning(): Boolean;
var
  Exe: String;
  F: TFileStream;
begin
  Result := False;
  Exe := ExpandConstant('{app}\{#AppExe}');
  if not FileExists(Exe) then Exit;
  try
    F := TFileStream.Create(Exe, fmOpenReadWrite or fmShareExclusive);
    F.Free;
  except
    Result := True;
  end;
end;

function WaitForAppToClose(const Msg: String): Boolean;
begin
  Result := True;
  while AppRunning() do
    if SuppressibleMsgBox(FmtMessage(Msg, ['FPS Overlay']), mbError, MB_OKCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not WaitForAppToClose(SetupMessage(msgSetupAppRunningError)) then
  begin
    // Only the first sentence: the rest says to click OK, and this page has no OK button.
    Result := FmtMessage(SetupMessage(msgSetupAppRunningError), ['FPS Overlay']);
    if Pos(#13, Result) > 0 then
      Result := Copy(Result, 1, Pos(#13, Result) - 1);
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForAppToClose(SetupMessage(msgUninstallAppRunningError));
end;

// Removes the "Start with Windows" task, but only when it starts this copy. The app made it as
// administrator, so deleting it may need a prompt; that only happens when the task exists.
procedure RemoveStartupTask();
var
  Schtasks, Xml: String;
  Output: TExecOutput;
  Code, I: Integer;
begin
  Schtasks := ExpandConstant('{sys}\schtasks.exe');
  if not ExecAndCaptureOutput(Schtasks, '/Query /TN "{#StartupTask}" /XML', '', SW_HIDE, ewWaitUntilTerminated, Code, Output) or (Code <> 0) then
    Exit;
  Xml := '';
  for I := 0 to GetArrayLength(Output.StdOut) - 1 do
    Xml := Xml + Output.StdOut[I];
  if Pos(Lowercase(ExpandConstant('{app}\{#AppExe}')), Lowercase(Xml)) = 0 then
    Exit;
  if Exec(Schtasks, '/Delete /F /TN "{#StartupTask}"', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
    Exit;
  ShellExec('runas', Schtasks, '/Delete /F /TN "{#StartupTask}"', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveStartupTask();
end;
