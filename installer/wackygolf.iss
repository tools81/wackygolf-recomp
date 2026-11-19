; Inno Setup script for The Wacky World of Miniature Golf static recompilation.
; Built by installer\make-release.ps1, which passes:
;   /DAppVersion=1.0.0  /DStage=<folder with the files to install>  /DOutDir=<output folder>
;
; The installer copies the recompiler, runtime and scripts, asks for the
; user's disc image, writes rom.cfg and builds wackygolf.exe on this PC.
; It installs per user (no administrator rights) into a folder the game can
; write to: saves, settings and the log stay in saves\ next to the program.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\dist\stage"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

#define AppName "Wacky World of Miniature Golf (Recomp)"
#define LaunchArgs '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\scripts\launch.ps1""'

[Setup]
AppId={{6E4C2F2B-8A47-4E3C-9C55-2B7F3E1D9A10}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=tools81
AppPublisherURL=https://github.com/tools81/wackygolf-recomp
AppUpdatesURL=https://github.com/tools81/wackygolf-recomp/releases
PrivilegesRequired=lowest
DefaultDirName={autopf}\WackyGolfRecomp
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
OutputDir={#OutDir}
OutputBaseFilename=wackygolf-recomp-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Excludes: "\VERSION"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#Stage}\VERSION"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: WriteRomCfg

[Dirs]
Name: "{app}\saves"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "{#LaunchArgs}"; WorkingDir: "{app}"; IconFilename: "{app}\wackygolf.exe"
Name: "{group}\Rebuild the game"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\build-game.ps1"""; WorkingDir: "{app}"
Name: "{group}\Saves folder"; Filename: "{app}\saves"
Name: "{group}\Uninstall"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "{#LaunchArgs}"; WorkingDir: "{app}"; IconFilename: "{app}\wackygolf.exe"; Tasks: desktopicon

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\build-game.ps1"""; WorkingDir: "{app}"; StatusMsg: "Building the game from your disc (a few minutes)..."; Flags: waituntilterminated
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "{#LaunchArgs}"; WorkingDir: "{app}"; Description: "Play now"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; generated on this PC; saves\ is kept unless the user removes it
Type: filesandordirs; Name: "{app}\gen"
Type: filesandordirs; Name: "{app}\extracted"
Type: filesandordirs; Name: "{app}\build"
Type: filesandordirs; Name: "{app}\tools\__pycache__"
Type: files; Name: "{app}\wackygolf.exe"
Type: files; Name: "{app}\rom.cfg"

[Code]
var
  RomPage: TInputFileWizardPage;

{ rom=<path> from an existing rom.cfg (reinstall / upgrade) }
function ExistingRomPath(): String;
var
  Lines: TArrayOfString;
  I: Integer;
  S: String;
begin
  Result := '';
  if LoadStringsFromFile(ExpandConstant('{app}\rom.cfg'), Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
    begin
      S := Trim(Lines[I]);
      if (Length(S) > 4) and (Lowercase(Copy(S, 1, 4)) = 'rom=') then
      begin
        Result := Copy(S, 5, Length(S));
        if (Length(Result) > 1) and (Result[1] = '"') then
          Result := Copy(Result, 2, Length(Result) - 2);
        Exit;
      end;
    end;
end;

{ the .bin a .cue refers to }
function CueToBin(const Path: String): String;
var
  Lines: TArrayOfString;
  I, P: Integer;
  S: String;
begin
  Result := Path;
  if Lowercase(ExtractFileExt(Path)) <> '.cue' then Exit;
  if not LoadStringsFromFile(Path, Lines) then Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    S := Trim(Lines[I]);
    if Uppercase(Copy(S, 1, 4)) = 'FILE' then
    begin
      P := Pos('"', S);
      if P > 0 then
      begin
        S := Copy(S, P + 1, Length(S));
        P := Pos('"', S);
        if P > 0 then
        begin
          Result := AddBackslash(ExtractFilePath(Path)) + Copy(S, 1, P - 1);
          Exit;
        end;
      end;
    end;
  end;
end;

{ raw 2352-byte-sector CD-i image: "CD-I " at sector 16, offset 25 }
function IsCdiImage(const Path: String): Boolean;
var
  Bin: String;
  F: TFileStream;
  Buf: AnsiString;
begin
  Result := False;
  Bin := CueToBin(Path);
  if not FileExists(Bin) then Exit;
  try
    F := TFileStream.Create(Bin, fmOpenRead or fmShareDenyNone);
    try
      if F.Size < 16 * 2352 + 30 then Exit;
      F.Seek(16 * 2352 + 25, soFromBeginning);
      SetLength(Buf, 5);
      F.ReadBuffer(Buf, 5);
      Result := (Buf = 'CD-I ');
    finally
      F.Free;
    end;
  except
    Result := False;
  end;
end;

procedure InitializeWizard();
begin
  RomPage := CreateInputFilePage(wpSelectDir,
    'Game disc image',
    'Where is your copy of the game?',
    'The game files are not included. Select the disc image of The Wacky World of ' +
    'Miniature Golf with Eugene Levy (Philips CD-i): the .cue file, or the .bin ' +
    '(raw 2352-byte sectors). The game is built from it on this PC, which needs ' +
    'Python 3, CMake and the Visual Studio 2022 Build Tools; Setup offers to ' +
    'install any that are missing.');
  RomPage.Add('Disc image (.cue or .bin):',
    'CD-i disc image (*.cue;*.bin)|*.cue;*.bin|All files|*.*', '.cue');
  { unattended installs: setup.exe /VERYSILENT /ROM="D:\Games\wacky.cue" }
  RomPage.Values[0] := ExpandConstant('{param:ROM|}');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = RomPage.ID) and (RomPage.Values[0] = '') then
    RomPage.Values[0] := ExistingRomPath();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = RomPage.ID then
  begin
    if Trim(RomPage.Values[0]) = '' then
    begin
      MsgBox('Select the game''s disc image to continue.', mbError, MB_OK);
      Result := False;
    end
    else if not IsCdiImage(Trim(RomPage.Values[0])) then
    begin
      MsgBox('That file is not a raw CD-i disc image (or its .bin is missing).' + #13#10 +
             'Select the game''s .cue or .bin.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ rom.cfg is read by the build in [Run] and by the game, so it is written as
  soon as the files are copied ([Run] entries run before ssPostInstall) }
procedure WriteRomCfg();
begin
  if Trim(RomPage.Values[0]) <> '' then
    SaveStringToFile(ExpandConstant('{app}\rom.cfg'), 'rom=' + Trim(RomPage.Values[0]) + #13#10, False);
end;
