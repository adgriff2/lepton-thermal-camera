# One-time setup per PC. Run in an ELEVATED PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\install.ps1 -Board "Dual RS232-HS A"
# -Board: the FTDI board the camera may use (list them with: .\lepton_vcam.exe boards). FTDI's default names are
# shared by many devices, so the camera never guesses: without -Board it uses no board at all. Given once, it's kept.
# Copies the camera to C:\ProgramData\LeptonVCam, creates the frame feed, registers the DLL (HKLM).
# Safe to run again after a rebuild. It does not add the camera itself: that is "lepton_vcam.exe install", without admin.
param([string]$Board)
$ErrorActionPreference = 'Stop'
$target = Join-Path $env:ProgramData 'LeptonVCam'
$feedSize = 3690496   # LEPTON_FEED_FILE_SIZE in shared\LeptonFeed.h

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Output 'FAILED: run this in an elevated PowerShell (Run as administrator).'; exit 1 }
if ([Environment]::OSVersion.Version.Build -lt 22000) { Write-Output 'FAILED: Windows 11 is required.'; exit 1 }
if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { Write-Output 'FAILED: 64-bit (x64) Windows and PowerShell are required.'; exit 1 }

# files are next to this script (package) or in .\dist (development folder)
$source = $PSScriptRoot
if (-not (Test-Path (Join-Path $source 'LeptonVCamSource.dll'))) { $source = Join-Path $PSScriptRoot 'dist' }
$files = 'LeptonVCamSource.dll', 'lepton_vcam.exe', 'vcam_selftest.exe', 'uninstall.ps1', 'NOTICES.txt'
foreach ($f in $files) { if (-not (Test-Path (Join-Path $source $f))) { Write-Output "FAILED: $f not found, run build.ps1 first."; exit 1 } }
if ($Board -and ($Board.Length -gt 63 -or $Board -match '[\x00-\x1F]')) { Write-Output 'FAILED: -Board must be the board name as listed by "lepton_vcam.exe boards".'; exit 1 }

# System services load this DLL, so ordinary users must not be able to change this folder.
# SY=SYSTEM BA=Administrators BU=Users LS=LocalService (Frame Server) AC=app packages IU=signed-in users
$dirSddl  = 'O:BAG:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;LS)(A;OICI;0x1200a9;;;AC)'
$feedSddl = 'O:BAG:BAD:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x12019f;;;IU)(A;;0x120089;;;BU)(A;;0x120089;;;LS)(A;;0x120089;;;AC)'

function Stop-Cameras {
    foreach ($n in 'FrameServer', 'FrameServerMonitor') {
        $s = Get-Service $n -ErrorAction SilentlyContinue
        if ($s -and $s.Status -ne 'Stopped') { Stop-Service $n -Force; (Get-Service $n).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) }
    }
}

if (Test-Path $target) {
    $item = Get-Item $target -Force
    $owner = (Get-Acl $target).GetOwner([Security.Principal.SecurityIdentifier]).Value
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $owner -notin 'S-1-5-32-544', 'S-1-5-18') {
        Write-Output "FAILED: $target already exists but was not created by this installer (owner $owner). Delete it and run again."; exit 1
    }
}
New-Item -ItemType Directory -Force $target | Out-Null
$acl = New-Object System.Security.AccessControl.DirectorySecurity
$acl.SetSecurityDescriptorSddlForm($dirSddl)
Set-Acl -Path $target -AclObject $acl

Stop-Cameras   # they may have the old DLL loaded
foreach ($f in $files) {
    for ($try = 1; ; $try++) {
        try { Copy-Item (Join-Path $source $f) $target -Force; break }
        catch {
            if ($try -ge 10) { Write-Output "FAILED: cannot replace $f, close every app that uses a camera and run again. ($($_.Exception.Message))"; exit 1 }
            Start-Sleep -Milliseconds 500; Stop-Cameras
        }
    }
}

foreach ($f in $files) { Unblock-File (Join-Path $target $f) }   # a downloaded zip marks its files, and services may refuse a marked DLL

$feedDir = Join-Path $target 'feed'
$feed = Join-Path $feedDir 'frame.bin'
New-Item -ItemType Directory -Force $feedDir | Out-Null
if (-not (Test-Path $feed) -or (Get-Item $feed -Force).Length -ne $feedSize) {
    $fs = [IO.File]::Create($feed)
    try { $fs.Write((New-Object byte[] $feedSize), 0, $feedSize) } finally { $fs.Close() }
}
[IO.File]::SetAttributes($feed, [IO.FileAttributes]::Temporary)   # asks Windows to keep it in memory
$acl = New-Object System.Security.AccessControl.FileSecurity
$acl.SetSecurityDescriptorSddlForm($feedSddl)
Set-Acl -Path $feed -AclObject $acl

$dll = Join-Path $target 'LeptonVCamSource.dll'
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s `"$dll`"" -Wait -PassThru
if ($p.ExitCode) { Write-Output "FAILED: regsvr32 exit code $($p.ExitCode)"; exit 1 }

$settings = 'HKLM:\SOFTWARE\LeptonVCam'   # read by the camera DLL; only administrators can change it
if ($Board) { New-Item $settings -Force | Out-Null; Set-ItemProperty $settings -Name Board -Value $Board }
$configured = (Get-ItemProperty $settings -ErrorAction SilentlyContinue).Board

$reg = (Get-ItemProperty 'HKLM:\SOFTWARE\Classes\CLSID\{5d5c12fe-863c-4d8a-8b21-79f4a8b9419e}\InprocServer32').'(default)'
"registered: $reg"
"feed: $feed ($((Get-Item $feed -Force).Length) bytes)"
if ($configured) { "board: $configured" }
else {
    'board: NOT CONFIGURED - the camera will show NO BOARD CONFIGURED. FTDI boards connected now:'
    & (Join-Path $target 'lepton_vcam.exe') boards | ForEach-Object { "  $_" }
    'Run this script again with -Board "<name from the list>" (for example -Board "Dual RS232-HS A").'
}
'INSTALL OK. Now, in a NORMAL (not elevated) PowerShell:'
"  & `"$target\lepton_vcam.exe`" install; & `"$target\lepton_vcam.exe`" status"
