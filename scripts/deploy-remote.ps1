#Requires -Version 5.1
<#
.SYNOPSIS
  SSH-only deploy: scp docker-compose.yml + .env.example, seed .env, docker compose pull/up. No Ansible required.

.EXAMPLE
  .\scripts\deploy-remote.ps1 -RemoteHost 192.168.1.50 -User pi

.EXAMPLE
  .\scripts\deploy-remote.ps1 192.168.1.50 pi -SkipPull
#>
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string] $RemoteHost,
  [Parameter(Position = 1)]
  [string] $User = "pi",
  [string] $Dir = "/opt/dronebros",
  [string] $Identity = "",
  [switch] $SkipPull
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$compose = Join-Path $RepoRoot "docker-compose.yml"
$envEx = Join-Path $RepoRoot ".env.example"
if (-not (Test-Path $compose)) { throw "Missing docker-compose.yml" }
if (-not (Test-Path $envEx)) { throw "Missing .env.example" }

$sshOpts = @("-o", "StrictHostKeyChecking=accept-new")
if ($Identity) {
  $sshOpts += "-i"
  $sshOpts += (Resolve-Path $Identity).Path
}
$dest = "${User}@${RemoteHost}"

& ssh @sshOpts $dest "mkdir -p '$Dir'"
& scp @sshOpts $compose "${dest}:${Dir}/docker-compose.yml"
& scp @sshOpts $envEx "${dest}:${Dir}/.env.example"
& ssh @sshOpts $dest "test -f '${Dir}/.env' || cp '${Dir}/.env.example' '${Dir}/.env'"

if ($SkipPull) {
  & ssh @sshOpts $dest "cd '$Dir' && docker compose up -d && docker compose ps"
} else {
  & ssh @sshOpts $dest "cd '$Dir' && docker compose pull && docker compose up -d && docker compose ps"
}
