# Local release (same result as the GitHub "Build" workflow, without Actions):
#   tools\release.ps1 -Version 0.2.0            build, package, publish a pre-release on GitHub
#   tools\release.ps1 -Version 0.2.0 -Final     publish as a normal (latest) release
#   tools\release.ps1 -Version 0.2.0 -DryRun    only build and package (dist\MWTogether-0.2.0.asi)
# Needs: Visual Studio 2022 C++ (x86), and the GitHub CLI (gh) logged in for publishing.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Final,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not $DryRun) {
    if (git status --porcelain) { throw "Uncommitted changes - commit or stash them first." }
    if (git tag --list "v$Version") { throw "Tag v$Version already exists." }
}

# 1. build with the version stamped in; MWT_RELEASE leaves the test commands and debug tools out
$env:MWT_VERSION = $Version
$env:MWT_RELEASE = '1'
& (Join-Path $root "build.bat")
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item Env:MWT_VERSION, Env:MWT_RELEASE

# 2. package: the release is the one file MWTogether.asi (no zip, no dinput8.dll). The menu logo, the
# default MWTogether.ini (written on the first start, never over an existing one), LICENSE and
# THIRD-PARTY-NOTICES are built into it as resources (src\version.rc).
# Ultimate ASI Loader is a required separate download: see README Install.
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$asi = Join-Path $dist "MWTogether-$Version.asi"
Copy-Item build\MWTogether.asi $asi -Force
Write-Host "Packaged $asi"

# SHA256 for the release notes (anyone can check a download against it), and links to the texts in the repository
$repo = 'https://github.com/StCost/nfs-most-wanted-together'
$notes = Join-Path $dist "MWTogether-$Version-notes.md"
$lines = @('### SHA256', '', '| File | SHA256 |', '|---|---|')
$lines += "| MWTogether.asi | ``$((Get-FileHash $asi -Algorithm SHA256).Hash.ToLower())`` |"
$lines += '', 'Put `MWTogether.asi` in `scripts\` next to `speed.exe`. Ultimate ASI Loader is not included: install the x86/Win32 dinput8.dll next to speed.exe (see README).'
$lines += '', "[README]($repo/blob/v$Version/README.md) - [LICENSE]($repo/blob/v$Version/LICENSE) (also built into MWTogether.asi) - [THIRD-PARTY-NOTICES]($repo/blob/v$Version/THIRD-PARTY-NOTICES.txt)"
Set-Content $notes $lines -Encoding utf8
Get-Content $notes | Write-Host
# unversioned name (README / CI); DryRun also writes it, so a local package is ready without tagging
$named = Join-Path $dist 'MWTogether.asi'
Copy-Item $asi $named -Force
if ($DryRun) { return }

# 3. tag + GitHub release: one asset with a fixed name, so README can link releases/latest/download/MWTogether.asi
git tag "v$Version"
git push origin "v$Version"
$flags = @('--notes-file', $notes, '--generate-notes', '--title', "NFS MW Together $Version")
if (-not $Final) { $flags += '--prerelease' }
gh release create "v$Version" $named @flags
Write-Host "Released v$Version"
