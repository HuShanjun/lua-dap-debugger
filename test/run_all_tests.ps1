# Run all regression tests under test/ (and C smoke bins in bin/).
# Usage (from repo root or anywhere):
#   .\test\run_all_tests.ps1
#   .\test\run_all_tests.ps1 -SkipC
#   .\test\run_all_tests.ps1 -Only dap
#   .\test\run_all_tests.ps1 -List

param(
    [switch]$SkipC,
    [string]$Only = "",
    [switch]$List
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$pyArgs = @()
if ($SkipC) { $pyArgs += "--skip-c" }
if ($Only) { $pyArgs += @("--only", $Only) }
if ($List) { $pyArgs += "--list" }

& python (Join-Path $PSScriptRoot "run_all_tests.py") @pyArgs
exit $LASTEXITCODE
