# Imports rtx-hdr-desktop.nip (RTX HDR on for rtx-hdr-desktop.exe) into the NVIDIA driver
# with NVIDIA Profile Inspector. Run it once; restart RTX HDR Desktop twice afterwards
# (the first start right after an import doesn't pick the profile up).
param(
    [string]$ProfileInspector
)
$ErrorActionPreference = 'Stop'
$nip = Join-Path $PSScriptRoot 'rtx-hdr-desktop.nip'

if (-not $ProfileInspector) {
    $guesses = @(
        (Join-Path $PSScriptRoot 'nvidiaProfileInspector.exe'),
        (Join-Path $PSScriptRoot 'nvidiaProfileInspector\nvidiaProfileInspector.exe'),
        "$env:USERPROFILE\Downloads\nvidiaProfileInspector\nvidiaProfileInspector.exe",
        "$env:USERPROFILE\Downloads\nvidiaProfileInspector.exe"
    )
    $ProfileInspector = $guesses | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $ProfileInspector -or -not (Test-Path -LiteralPath $ProfileInspector)) {
    throw "NVIDIA Profile Inspector not found. Download it from https://github.com/Orbmu2k/nvidiaProfileInspector/releases and run: .\install-profile.ps1 -ProfileInspector <path to nvidiaProfileInspector.exe>"
}

$p = Start-Process -FilePath $ProfileInspector -ArgumentList "`"$nip`" -silent" -Wait -PassThru
if ($p.ExitCode) { throw "Profile Inspector exited with $($p.ExitCode)" }
'Imported the "RTX HDR Desktop" profile (RTX HDR on for rtx-hdr-desktop.exe).'
