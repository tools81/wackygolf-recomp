# Shared helpers for setup.ps1, build-game.ps1 and launch.ps1.
# Everything stays inside the application folder (the folder above scripts\):
# the game writes saves, settings and its log to saves\ next to wackygolf.exe.

$ErrorActionPreference = 'Stop'
$script:AppDir = Split-Path -Parent $PSScriptRoot
$script:Repo = 'tools81/wackygolf-recomp'
$script:ExeName = 'wackygolf.exe'
$script:RomCfg = Join-Path $AppDir 'rom.cfg'
$script:BuiltStamp = Join-Path $AppDir 'build\built-version.txt'

function Get-AppVersion {
    $f = Join-Path $AppDir 'VERSION'
    if (Test-Path $f) { return (Get-Content $f -TotalCount 1).Trim() }
    return '0.0.0'
}

# ---------------------------------------------------------------- disc image

# A .cue names its .bin; return the .bin path (or the input if it is a .bin).
function Resolve-DiscBin([string]$Path) {
    if ($Path -match '\.cue$') {
        foreach ($line in Get-Content -LiteralPath $Path) {
            if ($line -match '^\s*FILE\s+"([^"]+)"') {
                return Join-Path (Split-Path -Parent $Path) $Matches[1]
            }
        }
    }
    return $Path
}

# Raw 2352-byte-sector CD-i image: the disc label "CD-I " at sector 16.
function Test-CdiImage([string]$Path) {
    try {
        $bin = Resolve-DiscBin $Path
        if (-not (Test-Path -LiteralPath $bin)) { return $false }
        $fs = [System.IO.File]::OpenRead($bin)
        try {
            $fs.Seek(16 * 2352 + 25, 'Begin') | Out-Null
            $buf = New-Object byte[] 5
            if ($fs.Read($buf, 0, 5) -ne 5) { return $false }
            return ([System.Text.Encoding]::ASCII.GetString($buf) -eq 'CD-I ')
        } finally { $fs.Dispose() }
    } catch { return $false }
}

function Get-RomPath {
    if (Test-Path $RomCfg) {
        foreach ($line in [System.IO.File]::ReadAllLines($RomCfg, [System.Text.Encoding]::Default)) {
            if ($line -match '^\s*rom\s*=\s*"?([^"]+?)"?\s*$') { return $Matches[1] }
        }
    }
    return $null
}

function Set-RomPath([string]$Path) {
    # system code page (ANSI), like the installer: the game opens the path with ANSI file APIs
    [System.IO.File]::WriteAllText($RomCfg, "rom=$Path`r`n", [System.Text.Encoding]::Default)
}

function Select-RomPath {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Select the Wacky World of Miniature Golf disc image (.cue or .bin)'
    $dlg.Filter = 'CD-i disc image (*.cue;*.bin)|*.cue;*.bin|All files (*.*)|*.*'
    while ($true) {
        if ($dlg.ShowDialog() -ne 'OK') { return $null }
        if (Test-CdiImage $dlg.FileName) { return $dlg.FileName }
        [System.Windows.Forms.MessageBox]::Show(
            "That file is not a raw CD-i disc image (2352-byte sectors).`n`nChoose the game's .cue or .bin.",
            'Wacky Golf', 'OK', 'Warning') | Out-Null
    }
}

# ---------------------------------------------------------------- build tools

function Update-PathFromRegistry {
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u"
}

# A working Python 3 (not the Microsoft Store placeholder).
function Find-Python {
    foreach ($cand in @(@('py', '-3'), @('python'), @('python3'))) {
        $cmd = Get-Command $cand[0] -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $args_ = @($cand | Select-Object -Skip 1) + @('-c', 'import sys; print(sys.executable) if sys.version_info >= (3, 8) else sys.exit(1)')
        try {
            $exe = & $cmd.Source @args_ 2>$null
            if ($LASTEXITCODE -eq 0 -and $exe -and (Test-Path $exe)) { return $exe.Trim() }
        } catch {}
    }
    return $null
}

function Find-VsPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -latest 2>$null
    if ($p) { return $p.Trim() }
    return $null
}

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vs = Find-VsPath
    if ($vs) {
        $c = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $c) { return $c }
    }
    return $null
}

function Get-MissingTools {
    $missing = @()
    if (-not (Find-Python)) { $missing += 'Python 3' }
    if (-not (Find-VsPath)) { $missing += 'Visual Studio 2022 Build Tools (C++)' }
    if (-not (Find-CMake)) { $missing += 'CMake' }
    return $missing
}

# Install missing tools with winget (Visual Studio asks for administrator rights).
function Install-MissingTools {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw 'winget is not available. Install Python 3, CMake and the Visual Studio 2022 Build Tools (C++ workload) by hand, then run Setup again.'
    }
    $common = @('--exact', '--accept-package-agreements', '--accept-source-agreements', '--silent')
    if (-not (Find-Python)) {
        Write-Host 'Installing Python 3...'
        winget install --id Python.Python.3.12 --scope user @common
    }
    if (-not (Find-VsPath)) {
        Write-Host 'Installing Visual Studio 2022 Build Tools (C++ workload, several GB; this takes a while)...'
        winget install --id Microsoft.VisualStudio.2022.BuildTools @common --override '--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
    }
    Update-PathFromRegistry
    if (-not (Find-CMake)) {
        Write-Host 'Installing CMake...'
        winget install --id Kitware.CMake @common
    }
    Update-PathFromRegistry
}

# ---------------------------------------------------------------- build

# Recompile the game from the disc image in rom.cfg.  Writes wackygolf.exe
# only if the whole build succeeds, so a failed build keeps the old one.
function Invoke-GameBuild {
    $py = Find-Python
    $cmake = Find-CMake
    if (-not $py -or -not $cmake -or -not (Find-VsPath)) {
        throw ('Missing build tools: ' + ((Get-MissingTools) -join ', '))
    }
    $rom = Get-RomPath
    if (-not $rom -or -not (Test-CdiImage $rom)) { throw "No valid disc image set in rom.cfg ($rom)." }
    $env:PYTHON = $py
    $env:RECOMP_ROM = $rom
    $env:Path = (Split-Path -Parent $cmake) + ';' + $env:Path
    # build.bat changes to its own folder
    $bat = Join-Path $AppDir 'build.bat'
    & cmd.exe /d /c "call `"$bat`""
    if ($LASTEXITCODE -ne 0) { throw "build.bat failed (exit code $LASTEXITCODE)." }
    if (-not (Test-Path (Join-Path $AppDir $ExeName))) { throw "$ExeName was not produced." }
    Set-Content -LiteralPath $BuiltStamp -Value (Get-AppVersion) -Encoding ASCII
}

# The version the current wackygolf.exe was built from (build\built-version.txt).
function Get-BuiltVersion {
    if (Test-Path $BuiltStamp) { return (Get-Content $BuiltStamp -TotalCount 1).Trim() }
    return $null
}

# ---------------------------------------------------------------- updates

# The latest published release, or $null (offline, rate-limited, no releases).
function Get-LatestRelease {
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $r = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" `
            -Headers @{ 'User-Agent' = 'wackygolf-recomp-updater'; 'Accept' = 'application/vnd.github+json' } -TimeoutSec 8
        $v = $r.tag_name -replace '^v', ''
        $asset = $r.assets | Where-Object { $_.name -like '*-portable.zip' } | Select-Object -First 1
        if (-not $asset) { return $null }
        return [pscustomobject]@{ Version = [version]$v; Tag = $r.tag_name; Url = $asset.browser_download_url; Name = $asset.name }
    } catch { return $null }
}

# Download a release's portable zip and copy it over the application folder.
# Kept: saves\, rom.cfg, rom\, extracted\ (the user's data and disc files).
function Install-Release($Release) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("wackygolf-update-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp $Release.Name
        Write-Host "Downloading $($Release.Name)..."
        Invoke-WebRequest -Uri $Release.Url -OutFile $zip -UseBasicParsing -Headers @{ 'User-Agent' = 'wackygolf-recomp-updater' }
        $src = Join-Path $tmp 'x'
        Expand-Archive -LiteralPath $zip -DestinationPath $src
        # the zip has one top-level folder
        $top = Get-ChildItem $src -Directory | Select-Object -First 1
        if ($top -and -not (Test-Path (Join-Path $src 'VERSION'))) { $src = $top.FullName }
        if (-not (Test-Path (Join-Path $src 'VERSION'))) { throw 'The downloaded package has no VERSION file.' }
        & robocopy.exe $src $AppDir /E /NFL /NDL /NJH /NJS /NP /XD saves rom extracted /XF rom.cfg | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Copying the update failed (robocopy exit code $LASTEXITCODE)." }
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
