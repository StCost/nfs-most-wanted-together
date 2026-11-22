# Builds build\MWTogetherLauncher.exe and its pictures (build\launcher\banner.jpg, background.jpg).
# Used by tools\release.ps1 and the GitHub "Build" workflow, so both make the same files.
# The pictures are NOT embedded in the exe: a large embedded image makes the exe look packed to
# antivirus heuristics. The package puts them in scripts\launcher\, where the launcher loads them.
#   tools\build-launcher.ps1 -Version 1.6.18
param([string]$Version = "0.0.0")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
New-Item -ItemType Directory -Force build\launcher | Out-Null

# file details: a four-part number (1.6.18 -> 1.6.18.0; anything after a '-' is dropped)
$parts = @(($Version -split '-')[0] -split '\.' | ? { $_ -match '^\d+$' })
while ($parts.Count -lt 4) { $parts += '0' }
$lv = ($parts[0..3]) -join '.'
Set-Content build\LauncherVersion.cs "static class LauncherVersion { public const string Text = `"$lv`"; }"

# banner as a JPEG (the 1.3 MB PNG is only the source), background as is
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile((Resolve-Path docs\banner.png))
$enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | ? MimeType -eq 'image/jpeg'
$ep = New-Object System.Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 85L
$img.Save((Join-Path (Resolve-Path build\launcher) 'banner.jpg'), $enc, $ep); $img.Dispose()
Copy-Item docs\launcher-bg.jpg build\launcher\background.jpg -Force

& C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe /nologo /target:winexe /optimize+ /out:build\MWTogetherLauncher.exe /win32manifest:launcher\app.manifest /r:System.Windows.Forms.dll /r:System.Drawing.dll launcher\Launcher.cs build\LauncherVersion.cs
if ($LASTEXITCODE -ne 0) { throw "launcher build failed" }
Write-Host "Built build\MWTogetherLauncher.exe $lv (+ build\launcher\banner.jpg, background.jpg)"
