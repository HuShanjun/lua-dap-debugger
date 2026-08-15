$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$candidates = @(
    (Join-Path $root 'bin\lua-runner.exe'),
    (Join-Path $root 'bin\Debug\lua-runner.exe')
)
$src = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $src) {
    throw "lua-runner.exe not found under bin\; build target lua-runner first"
}
$dstDir = Join-Path $root 'vscode-extension\bin\win32-x64'
New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
$dst = Join-Path $dstDir 'lua-runner.exe'
Copy-Item -Force $src $dst
Write-Host "Copied $src -> $dst"
