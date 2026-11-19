# Start the game.  First checks GitHub for a newer release; if there is one,
# downloads it over this folder (keeping saves\ and rom.cfg) and rebuilds the
# game from the disc.  If the check, download or build fails, the installed
# version starts as before.
param([switch]$NoUpdate, [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs)
. (Join-Path $PSScriptRoot 'common.ps1')
Add-Type -AssemblyName System.Windows.Forms

$exe = Join-Path $AppDir $ExeName
# not built yet, or an earlier update's rebuild did not finish
$needBuild = -not (Test-Path $exe) -or (Get-BuiltVersion) -ne (Get-AppVersion)

if (-not $NoUpdate -and -not $env:WACKYGOLF_NO_UPDATE) {
    $rel = Get-LatestRelease
    $cur = [version](Get-AppVersion)
    if ($rel -and $rel.Version -gt $cur) {
        $msg = "Version $($rel.Version) is available (installed: $cur).`n`nIt is downloaded and the game is rebuilt from your disc (a few minutes). Your saves and settings are kept."
        [System.Windows.Forms.MessageBox]::Show($msg, 'Wacky Golf update', 'OK', 'Information') | Out-Null
        try {
            Install-Release $rel
            $needBuild = $true
        } catch {
            [System.Windows.Forms.MessageBox]::Show("The update could not be installed:`n$($_.Exception.Message)`n`nStarting version $cur.", 'Wacky Golf update', 'OK', 'Warning') | Out-Null
        }
    }
}

if ($needBuild) {
    # visible window: progress, prompts for missing tools
    $p = Start-Process powershell.exe -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'build-game.ps1'))
    if ($p.ExitCode -ne 0) {
        if (Test-Path $exe) {
            [System.Windows.Forms.MessageBox]::Show('Rebuilding after the update failed; starting the previous build. Run Setup to try again.', 'Wacky Golf', 'OK', 'Warning') | Out-Null
        } else {
            [System.Windows.Forms.MessageBox]::Show('The game could not be built. Run Setup to try again.', 'Wacky Golf', 'OK', 'Error') | Out-Null
            exit 1
        }
    }
}

# the game reads rom.cfg itself; it keeps saves, settings and its log in saves\
$startArgs = @{ FilePath = $exe; WorkingDirectory = $AppDir }
if ($GameArgs) { $startArgs.ArgumentList = $GameArgs }
Start-Process @startArgs
