# Package a release from the committed sources:
#   dist\wackygolf-recomp-<version>-portable.zip   (folder with Setup.cmd / Play.cmd)
#   dist\wackygolf-recomp-<version>-setup.exe      (Inno Setup installer)
# Only files tracked by git are packaged, so the disc image, the extracted
# game files, the generated code and saves can never end up in a release.
param([string]$Iscc)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Get-Content VERSION -TotalCount 1).Trim()
$name = "wackygolf-recomp-$version"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist "stage\$name"

if (git status --porcelain) { throw 'Uncommitted changes: commit first so the release matches the repository.' }

Remove-Item -Recurse -Force (Join-Path $dist 'stage') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# tracked files, minus the packaging and repository files themselves
$files = git ls-files | Where-Object { $_ -notmatch '^(installer/|\.git)' }
foreach ($f in $files) {
    $dst = Join-Path $stage $f
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $f) -Destination $dst
}
New-Item -ItemType Directory -Force (Join-Path $stage 'saves') | Out-Null

# safety net: nothing from the disc
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.bin', '.cue', '.exe', '.state' -or $_.FullName -match '\\(gen|extracted|rom)\\' }
if ($bad) { throw "Refusing to package game data: $($bad.FullName -join ', ')" }

$zip = Join-Path $dist "$name-portable.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "portable: $zip"

if (-not $Iscc) {
    $Iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
              "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) { throw 'Inno Setup (ISCC.exe) not found; install it with: winget install JRSoftware.InnoSetup' }
& $Iscc /Q "/DAppVersion=$version" "/DStage=$stage" "/DOutDir=$dist" (Join-Path $PSScriptRoot 'wackygolf.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
Write-Host "installer: $(Join-Path $dist "$name-setup.exe")"
