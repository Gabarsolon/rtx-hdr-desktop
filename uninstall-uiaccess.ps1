# Undoes install-uiaccess.ps1: removes the Program Files copy, the Start-with-Windows
# entry, and the self-signed certificate from every store it was added to.
# Run as administrator (it relaunches itself elevated if needed).
$ErrorActionPreference = 'Continue'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    return
}

# Stop a running installed copy.
Get-Process rtx-hdr-desktop -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like "$env:ProgramFiles\RTX HDR Desktop\*" } | Stop-Process -Force -ErrorAction SilentlyContinue

Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RTX HDR Desktop' -ErrorAction SilentlyContinue
$dir = Join-Path $env:ProgramFiles 'RTX HDR Desktop'
if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force; "Removed $dir" }

$subject = 'CN=RTX HDR Desktop (self-signed)'
foreach ($store in 'Cert:\LocalMachine\My', 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher') {
    Get-ChildItem $store -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq $subject } | ForEach-Object {
        Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue
        "Removed certificate from $store"
    }
}
'Done.'
