# Build wackygolf.exe from the disc image named in rom.cfg.
# Run by the installer after it copies the files, and by launch.ps1 after an
# update.  Offers to install missing build tools with winget.
param(
    [switch]$InstallTools,   # install missing tools without asking
    [switch]$NoPause         # do not wait for a key at the end
)
. (Join-Path $PSScriptRoot 'common.ps1')
$host.UI.RawUI.WindowTitle = 'Wacky Golf - building the game'

$code = 0
try {
    Write-Host "The Wacky World of Miniature Golf - static recompilation $(Get-AppVersion)"
    Write-Host "Folder: $AppDir"
    Write-Host ''

    $rom = Get-RomPath
    if (-not $rom -or -not (Test-CdiImage $rom)) {
        Write-Host 'Choose the game disc image (.cue or .bin)...'
        $rom = Select-RomPath
        if (-not $rom) { throw 'No disc image selected.' }
        Set-RomPath $rom
    }
    Write-Host "Disc image: $rom"

    $missing = Get-MissingTools
    if ($missing.Count) {
        Write-Host ''
        Write-Host 'The game is compiled on this PC from your disc, which needs:'
        $missing | ForEach-Object { Write-Host "  - $_" }
        $ok = $InstallTools
        if (-not $ok) {
            $a = Read-Host 'Install them now with winget? [Y/n]'
            $ok = ($a -eq '' -or $a -match '^[yY]')
        }
        if (-not $ok) { throw 'Build tools are missing.' }
        Install-MissingTools
        $missing = Get-MissingTools
        if ($missing.Count) { throw ('Still missing after installing: ' + ($missing -join ', ') + '. A restart of Windows may be needed; then run Setup again.') }
    }

    Write-Host ''
    Write-Host 'Building (a few minutes)...'
    Invoke-GameBuild
    Write-Host ''
    Write-Host 'Build complete.'
} catch {
    Write-Host ''
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $code = 1
}
if (-not $NoPause) {
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
}
exit $code
