#Requires -Version 5.1
<#
.SYNOPSIS
  Deploy stats + HMI via Ansible (deploy_stack.yml). Uses ansible-playbook on PATH, or WSL + scripts/deploy-stack.sh.

.EXAMPLE
  .\scripts\deploy-stack.ps1 -RemoteHost 192.168.1.50 -User pi

.EXAMPLE
  .\scripts\deploy-stack.ps1 192.168.1.50 pi -SkipPull
#>
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string] $RemoteHost,
  [Parameter(Position = 1)]
  [string] $User = "pi",
  [string] $Identity = "",
  [switch] $SkipPull
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Playbook = Join-Path $RepoRoot "deploy\ansible\playbooks\deploy_stack.yml"
$AnsibleCfg = Join-Path $RepoRoot "deploy\ansible\ansible.cfg"
$Inv = "${RemoteHost},"

if (-not (Test-Path $Playbook)) { throw "Missing playbook: $Playbook" }

$env:ANSIBLE_CONFIG = $AnsibleCfg
$skipVar = if ($SkipPull) { "true" } else { "false" }

function Invoke-AnsiblePlaybookLocal {
  $apArgs = @(
    $Playbook,
    "-i", $Inv,
    "-e", "ansible_user=$User",
    "-e", "skip_image_pull=$skipVar"
  )
  if ($Identity) { $apArgs = @("--private-key", (Resolve-Path $Identity).Path) + $apArgs }
  & ansible-playbook @apArgs
}

if (Get-Command ansible-playbook -ErrorAction SilentlyContinue) {
  Invoke-AnsiblePlaybookLocal
  exit $LASTEXITCODE
}

if (Get-Command wsl -ErrorAction SilentlyContinue) {
  $unixRepo = (wsl wslpath -a $RepoRoot).Trim()
  $frag = "./scripts/deploy-stack.sh '$RemoteHost' '$User'"
  if ($Identity) {
    $idWsl = (wsl wslpath -a (Resolve-Path $Identity).Path).Trim()
    $frag += " -i '$idWsl'"
  }
  if ($SkipPull) { $frag += " --skip-pull" }
  wsl bash -lc "cd '$unixRepo' && $frag"
  exit $LASTEXITCODE
}

throw "ansible-playbook not found and WSL not available. Install Ansible, enable WSL, or run scripts/deploy-remote.ps1 for SSH-only copy + compose."
