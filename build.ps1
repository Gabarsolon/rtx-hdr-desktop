# Builds rtx-hdr-desktop.exe (x64) into .\out (or -Out <dir>) with the newest Visual Studio found.
param([string]$Out)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = if ($Out) { $Out } else { Join-Path $root 'out' }
New-Item -ItemType Directory -Force $out | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'

Push-Location $out
try {
    cmd /c "`"$vcvars`" x64 >nul && rc /nologo /i `"$root`" /fo app.res `"$root\app.rc`" && cl /nologo /W3 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE `"$root\rtx-hdr-desktop.cpp`" app.res /Fe:rtx-hdr-desktop.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib dwmapi.lib d3d11.lib dxgi.lib d3dcompiler.lib dcomp.lib psapi.lib"
    if ($LASTEXITCODE) { throw "build failed" }
} finally { Pop-Location }
