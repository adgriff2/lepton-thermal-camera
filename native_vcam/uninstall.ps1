# Removes everything. First, as each user who added the camera (NORMAL PowerShell):
#   & "$env:ProgramData\LeptonVCam\lepton_vcam.exe" remove
# Then in an ELEVATED PowerShell:
#   powershell -ExecutionPolicy Bypass -File "$env:ProgramData\LeptonVCam\uninstall.ps1"
$ErrorActionPreference = 'Stop'
$target = Join-Path $env:ProgramData 'LeptonVCam'
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Output 'FAILED: run this in an elevated PowerShell (Run as administrator).'; exit 1 }
if (-not (Test-Path $target)) { Write-Output 'Nothing to remove.'; exit 0 }
if ((Get-Item $target -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { Write-Output "FAILED: $target is a link, not touching it."; exit 1 }

$exe = Join-Path $target 'lepton_vcam.exe'
if (Test-Path $exe) { & $exe remove }   # covers the account running this script
foreach ($n in 'FrameServer', 'FrameServerMonitor') {
    $s = Get-Service $n -ErrorAction SilentlyContinue
    if ($s -and $s.Status -ne 'Stopped') { Stop-Service $n -Force; (Get-Service $n).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) }
}
$dll = Join-Path $target 'LeptonVCamSource.dll'
if (Test-Path $dll) {
    $p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/u /s `"$dll`"" -Wait -PassThru
    if ($p.ExitCode) { Write-Output "note: regsvr32 /u exit code $($p.ExitCode)" }
}
Set-Location $env:SystemRoot
try { Remove-Item $target -Recurse -Force; 'UNINSTALL OK' }
catch { Write-Output "Unregistered, but some files are in use: close camera apps and the Lepton program, then delete $target. ($($_.Exception.Message))"; exit 1 }
