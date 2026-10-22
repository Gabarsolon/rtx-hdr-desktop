# Installs RTX HDR Desktop as a UIAccess app so it can stay above the Start menu,
# taskbar previews and system flyouts (otherwise they cover the overlay and the
# driver drops RTX HDR while they're open).
#
# What it does, and why each step is needed for UIAccess:
#   1. Copies the exe to C:\Program Files\RTX HDR Desktop  (UIAccess needs a
#      "secure location").
#   2. Swaps in the uiAccess="true" manifest with the Windows SDK's mt.exe.
#   3. Creates a self-signed code-signing certificate and trusts it on THIS
#      machine (UIAccess needs the exe signed by a trusted publisher).
#   4. Signs the installed exe with it.
#
# Security note: trusting a self-signed root certificate is a lasting change to
# what your machine trusts. Undo everything with uninstall-uiaccess.ps1.
#
# Run it as administrator (it relaunches itself elevated if needed).

param(
    [switch]$Startup,  # also start it when you sign in (Run key -> the installed exe)
    [string]$Exe       # exe to install; default: the newest build next to this script
)
$ErrorActionPreference = 'Stop'

# --- relaunch elevated ------------------------------------------------------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arglist = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Startup) { $argList += '-Startup' }
    if ($Exe) { $argList += @('-Exe', "`"$Exe`"") }
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    return
}

$root = $PSScriptRoot
if (-not $Exe) {
    $Exe = @("$root\out\next\rtx-hdr-desktop.exe", "$root\out\rtx-hdr-desktop.exe", "$root\rtx-hdr-desktop.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) { throw "rtx-hdr-desktop.exe not found. Build it, or pass -Exe <path>." }
$manifest = Join-Path $root 'app.uiaccess.manifest'
if (-not (Test-Path -LiteralPath $manifest)) { throw "app.uiaccess.manifest not found next to this script." }

# --- find the Windows SDK's signtool.exe and mt.exe -------------------------
function Find-SdkTool($name) {
    $hit = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\$name" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $hit) { throw "$name not found. Install the Windows 10/11 SDK." }
    $hit.FullName
}
$signtool = Find-SdkTool 'signtool.exe'
$mt = Find-SdkTool 'mt.exe'

# --- 1. copy into Program Files ---------------------------------------------
$dir = Join-Path $env:ProgramFiles 'RTX HDR Desktop'
New-Item -ItemType Directory -Force $dir | Out-Null
$dest = Join-Path $dir 'rtx-hdr-desktop.exe'
Copy-Item -LiteralPath $Exe $dest -Force
foreach ($f in 'rtx-hdr-desktop.nip', 'install-profile.ps1', 'README.md') {
    if (Test-Path -LiteralPath "$root\$f") { Copy-Item -LiteralPath "$root\$f" $dir -Force }
}
"Copied to $dest"

# --- 2. swap in the uiAccess=true manifest ----------------------------------
& $mt -nologo -manifest $manifest -outputresource:"$dest;#1"
if ($LASTEXITCODE) { throw "mt.exe failed to set the manifest ($LASTEXITCODE)" }
'Set uiAccess="true" in the installed copy.'

# --- 3. self-signed code-signing cert, trusted on this machine --------------
$subject = 'CN=RTX HDR Desktop (self-signed)'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
        -KeyUsage DigitalSignature -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(10) `
        -FriendlyName 'RTX HDR Desktop code signing'
    "Created a code-signing certificate ($($cert.Thumbprint))."
} else {
    "Reusing the existing certificate ($($cert.Thumbprint))."
}
# Trust it (and its signatures) on this machine. UIAccess validates the chain.
$pub = Join-Path $env:TEMP 'rtx-hdr-desktop-cert.cer'
Export-Certificate -Cert $cert -FilePath $pub | Out-Null
Import-Certificate -FilePath $pub -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $pub -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Remove-Item $pub -Force
'Trusted the certificate (LocalMachine Root + TrustedPublisher).'

# --- 4. sign the installed exe ----------------------------------------------
& $signtool sign /sha1 $cert.Thumbprint /fd SHA256 $dest
if ($LASTEXITCODE) {
    try { & $signtool sign /sha1 $cert.Thumbprint /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $dest } catch {}
}
& $signtool verify /pa $dest
if ($LASTEXITCODE) { throw "signature did not verify ($LASTEXITCODE)" }
'Signed and verified.'

if ($Startup) {
    $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    New-ItemProperty -Path $run -Name 'RTX HDR Desktop' -Value "`"$dest`"" -PropertyType String -Force | Out-Null
    'Added to Start with Windows.'
}

Write-Host ''
Write-Host "Installed. Close any running copy, then start:" -ForegroundColor Green
Write-Host "  `"$dest`""
Write-Host "In the tray menu, tick 'Stay above the Start menu and popups'."
