param(
    [string]$Python = "3.11",
    [string]$ZipName = "OCIOBakeLUTs.zip"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

$BuildDir = Join-Path $ScriptDir "build"
$DistDir = Join-Path $ScriptDir "dist"
$AppDir = Join-Path $DistDir "OCIOBakeLUTs"
$ZipPath = Join-Path $DistDir $ZipName

Remove-Item -Recurse -Force $BuildDir, $AppDir, $ZipPath -ErrorAction SilentlyContinue

uv run --python $Python --with pyinstaller --with numpy --with opencolorio pyinstaller --noconfirm --clean OCIOBakeLUTsGUI.spec

if (-not (Test-Path $AppDir)) {
    throw "PyInstaller did not produce expected directory: $AppDir"
}

if (-not (Test-Path $DistDir)) {
    New-Item -ItemType Directory -Path $DistDir | Out-Null
}

Compress-Archive -Path $AppDir -DestinationPath $ZipPath -Force
Write-Host "Wrote $ZipPath"
