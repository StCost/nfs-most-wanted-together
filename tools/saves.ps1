# Switch NFS Most Wanted between the real save and a fresh test save. Close the game first; nothing is deleted.
#   saves.ps1 status | fresh | real
#   saves.ps1 snapshot-fresh [-Force]  - keep the active fresh save as the template for the next 'fresh'
param(
  [Parameter(Mandatory)][ValidateSet('status','fresh','real','snapshot-fresh')][string]$Mode,
  [string]$Docs = [Environment]::GetFolderPath('MyDocuments'),
  [string]$Template,
  [switch]$Force
)
$active = Join-Path $Docs 'NFS Most Wanted'
$real   = Join-Path $Docs 'NFS Most Wanted.real'
$fresh  = Join-Path $Docs 'NFS Most Wanted.fresh'
if (-not $Template) { $Template = Join-Path $Docs 'NFS Most Wanted.fresh-template' }
$Template = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Template)
$marker = '.coop-save-kind'
$ErrorActionPreference = 'Stop'
if ($Mode -ne 'status' -and (Get-Process speed, speed2 -ErrorAction SilentlyContinue)) { 'Close the game first.'; exit 1 }
function Kind($dir) { if (Test-Path "$dir\$marker") { "$(Get-Content "$dir\$marker" -Raw)".Trim() } elseif (Test-Path $dir) { 'real' } else { 'none' } }
function Ren($from, $to) { Rename-Item $from (Split-Path $to -Leaf) }
switch ($Mode) {
  'status' { "active: $(Kind $active)   (.real present: $(Test-Path $real), .fresh present: $(Test-Path $fresh))" }
  'fresh' {
    if ((Kind $active) -eq 'fresh') { 'fresh save already active'; break }
    # .real with no active folder = an earlier 'fresh' stopped halfway: carry on.
    if ((Test-Path $real) -and (Test-Path $active)) { "refusing: $real already exists"; exit 1 }
    if (Test-Path $active) { Ren $active $real }
    if (Test-Path $fresh) { Ren $fresh $active }
    elseif (Test-Path $Template) { Copy-Item $Template $active -Recurse }
    else { New-Item -ItemType Directory $active | Out-Null }
    Set-Content "$active\$marker" 'fresh'
    "fresh save active (real save kept at $real)"
  }
  'real' {
    if ((Kind $active) -eq 'real') { 'real save already active'; break }
    if (-not (Test-Path $real)) { "no $real to restore"; exit 1 }
    # .fresh with no active folder = an earlier 'real' stopped halfway: carry on.
    if ((Test-Path $fresh) -and (Test-Path $active)) { "refusing: $fresh already exists"; exit 1 }
    if (Test-Path $active) { Ren $active $fresh }
    Ren $real $active
    "real save active (fresh save kept at $fresh)"
  }
  'snapshot-fresh' {
    if (-not (Test-Path $active)) { "no $active to snapshot"; exit 1 }
    if ((Kind $active) -ne 'fresh' -and -not $Force) { 'refusing: the active save is the real one (use -Force)'; exit 1 }
    $old = "$Template-old"
    if (Test-Path $Template) {
      if (Test-Path $old) { Remove-Item $old -Recurse -Force }
      Ren $Template $old
    }
    $parent = Split-Path $Template
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory $parent | Out-Null }
    Copy-Item $active $Template -Recurse
    Set-Content "$Template\$marker" 'fresh'
    "snapshot saved to $Template"
  }
}
