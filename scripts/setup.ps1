# Setup for the portable zip: choose the disc image, build the game and
# optionally put a shortcut on the desktop.  Nothing is written outside this
# folder except that optional shortcut.
. (Join-Path $PSScriptRoot 'common.ps1')
Add-Type -AssemblyName System.Windows.Forms

$cur = Get-RomPath
if ($cur -and (Test-CdiImage $cur)) {
    $keep = [System.Windows.Forms.MessageBox]::Show("Use this disc image?`n`n$cur", 'Wacky Golf setup', 'YesNo', 'Question')
    if ($keep -ne 'Yes') { $cur = $null }
} else { $cur = $null }
if (-not $cur) {
    [System.Windows.Forms.MessageBox]::Show(
        "Choose your disc image of The Wacky World of Miniature Golf with Eugene Levy (CD-i).`n`nThe game files are not included; the game is built from your disc.",
        'Wacky Golf setup', 'OK', 'Information') | Out-Null
    $cur = Select-RomPath
    if (-not $cur) { exit 1 }
}
Set-RomPath $cur

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-game.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$a = [System.Windows.Forms.MessageBox]::Show('Create a desktop shortcut?', 'Wacky Golf setup', 'YesNo', 'Question')
if ($a -eq 'Yes') {
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Wacky World of Miniature Golf.lnk'))
    $lnk.TargetPath = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $lnk.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $PSScriptRoot 'launch.ps1') + '"'
    $lnk.WorkingDirectory = $AppDir
    $lnk.IconLocation = (Join-Path $AppDir $ExeName) + ',0'
    $lnk.Save()
}
[System.Windows.Forms.MessageBox]::Show('Setup is complete. Start the game with Play.cmd or the shortcut.', 'Wacky Golf setup', 'OK', 'Information') | Out-Null
