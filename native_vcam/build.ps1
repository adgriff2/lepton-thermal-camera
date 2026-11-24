# Builds everything into .\dist (no admin rights needed).
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 [-Configuration Debug]
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio is not installed.' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'The "Desktop development with C++" workload is not installed.' }

$sln = Join-Path $root 'LeptonVCam.sln'
& $msbuild $sln -t:Restore -p:RestorePackagesConfig=true -v:quiet -nologo
if ($LASTEXITCODE) { throw "package restore failed ($LASTEXITCODE)" }
& $msbuild $sln -t:Build "-p:Configuration=$Configuration" -p:Platform=x64 -m -v:minimal -nologo
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
foreach ($f in 'LeptonVCamSource.dll', 'lepton_vcam.exe', 'vcam_selftest.exe') { Copy-Item (Join-Path $root "out\$Configuration\$f") $dist -Force }
foreach ($f in 'install.ps1', 'uninstall.ps1', 'NOTICES.txt') {
    $p = Join-Path $root $f
    if (Test-Path $p) { Copy-Item $p $dist -Force }
}
Get-ChildItem $dist -File | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.Name }
"BUILD OK ($Configuration) -> $dist"
