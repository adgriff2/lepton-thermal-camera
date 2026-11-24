# Makes two zips for other x64 Windows 11 PCs (no admin rights needed here). Run build.ps1 first.
#   LeptonCamera.zip  the camera (needs admin once on the other PC)
#   LeptonViewer.zip  portable preview window (no admin; the Python program, without the camera)
#   powershell -ExecutionPolicy Bypass -File .\package.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
$venv = Join-Path $root 'obj\venv'
$work = Join-Path $root 'obj\pyinstaller'
$out  = Join-Path $root 'package\LeptonCamera'
$view = Join-Path $root 'package\LeptonViewer'
if (-not (Test-Path (Join-Path $dist 'LeptonVCamSource.dll'))) { throw 'run build.ps1 first' }

function Invoke-Checked([string]$exe, [string[]]$arguments) {
    # native tools write progress to stderr: judge them by exit code only
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $log = & $exe @arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE; $ErrorActionPreference = $old
    if ($code) { $log | Select-Object -Last 25; throw "$exe failed ($code)" }
}

# isolated environment with the same package versions as the working setup
$py = Join-Path $venv 'Scripts\python.exe'
if (-not (Test-Path $py)) { Invoke-Checked 'python' @('-m', 'venv', $venv) }
$versions = & python -c "import importlib.metadata as m; print(' '.join(f'{p}=={m.version(p)}' for p in ('numpy','opencv-python','ftd2xx')))"
if ($LASTEXITCODE) { throw 'numpy, opencv-python and ftd2xx must be installed' }
Invoke-Checked $py (@('-m', 'pip', 'install', '-q', '--disable-pip-version-check', 'pyinstaller') + $versions.Split(' '))

Invoke-Checked $py @('-m', 'PyInstaller', '--noconfirm', '--clean', '--onedir', '--console', '--name', 'lepton_cam',
    '--paths', (Join-Path $root 'python'), '--hidden-import', 'leptonfeed', '--exclude-module', 'pyvirtualcam',
    '--distpath', (Join-Path $work 'dist'), '--workpath', (Join-Path $work 'build'), '--specpath', $work,
    (Join-Path (Split-Path $root) 'lepton_cam.py'))

foreach ($d in $out, $view) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    New-Item -ItemType Directory -Force $d | Out-Null
}
foreach ($f in 'LeptonVCamSource.dll', 'lepton_vcam.exe', 'vcam_selftest.exe') { Copy-Item (Join-Path $dist $f) $out }
foreach ($f in 'install.ps1', 'uninstall.ps1', 'LICENSE') { Copy-Item (Join-Path $root $f) $out }
Copy-Item (Join-Path $work 'dist\lepton_cam') (Join-Path $view 'lepton') -Recurse
# OpenCV's video file reader/writer (FFmpeg, ~40 MB): lepton_cam.py never opens video files
Get-ChildItem (Join-Path $view 'lepton') -Recurse -Filter 'opencv_videoio_ffmpeg*.dll' | Remove-Item -Force

@'
@echo off
rem Adds the camera for the current user (no admin). install.ps1 must have been run on this PC before.
set "VC=%ProgramData%\LeptonVCam\lepton_vcam.exe"
if not exist "%VC%" echo Not installed on this PC yet: run install.ps1 as admin first (see README.txt).
if exist "%VC%" "%VC%" install
if exist "%VC%" "%VC%" status
pause
'@ | Set-Content (Join-Path $out 'Add camera.cmd') -Encoding ASCII

@'
FLIR Lepton camera for Windows 11 (x64)

1. Once per PC, in an ELEVATED PowerShell opened in this folder:
     powershell -ExecutionPolicy Bypass -File .\install.ps1
2. Once per user, without admin: double-click "Add camera.cmd".
3. Plug in the FT2232H board. Windows Update normally installs its driver
   (otherwise, as admin: https://ftdichip.com/drivers/d2xx-drivers/).

Then pick "FLIR Lepton" in the Windows Camera app or any other camera app. Nothing has to be started.
If there is no picture, the camera image says why (for example BOARD NOT CONNECTED).
The files are not signed: SmartScreen may warn, and Smart App Control or company policy may block them.

Remove: normal PowerShell:   & "$env:ProgramData\LeptonVCam\lepton_vcam.exe" remove
        elevated PowerShell: powershell -ExecutionPolicy Bypass -File "$env:ProgramData\LeptonVCam\uninstall.ps1"
'@ | Set-Content (Join-Path $out 'README.txt') -Encoding ASCII

@'
@echo off
"%~dp0lepton\lepton_cam.exe" --model 3 %*
pause
'@ | Set-Content (Join-Path $view 'Lepton viewer.cmd') -Encoding ASCII

@'
FLIR Lepton viewer for Windows (x64). No admin, no installation: double-click "Lepton viewer.cmd".
Keys in the window: q quit, p palette, f flip, m mirror. It is a window, not a camera: share the window in a call.
Needs the FTDI driver (Windows Update normally installs it when the board is plugged in).
If no picture appears within a few seconds, close it and start it again.
'@ | Set-Content (Join-Path $view 'README.txt') -Encoding ASCII

foreach ($d in $out, $view) {
    $zip = "$d.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    for ($try = 1; ; $try++) {   # antivirus may briefly lock the freshly built files
        try { Compress-Archive -Path $d -DestinationPath $zip -ErrorAction Stop; break }
        catch { if ($try -ge 5) { throw }; Start-Sleep 3; if (Test-Path $zip) { Remove-Item $zip -Force } }
    }
    'PACKAGE OK  {0:N1} MB  {1}' -f ((Get-Item $zip).Length / 1MB), $zip
}
