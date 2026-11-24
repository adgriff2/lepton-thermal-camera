# Starts the Lepton automatically while an app uses the "FLIR Lepton" camera. Normal PowerShell, per user, no admin:
#   powershell -ExecutionPolicy Bypass -File .\autostart.ps1 on | off | status
param([Parameter(Mandatory = $true)][ValidateSet('on', 'off', 'status')][string]$Command)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $env:LOCALAPPDATA 'LeptonVCam'
$agent = Join-Path $folder 'lepton_agent.exe'
$runKey = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$name = 'LeptonCameraAgent'

function Stop-Agent {
    if (Test-Path $agent) {
        $p = Start-Process $agent -ArgumentList 'stop' -Wait -PassThru
        if ($p.ExitCode) { throw "the running agent did not stop, see $folder\agent.log" }
    }
}

if ($Command -eq 'on') {
    # package: everything is next to this script. Development folder: .\dist and the Python sources.
    $source = Join-Path $PSScriptRoot 'lepton_agent.exe'
    if (-not (Test-Path $source)) { $source = Join-Path $PSScriptRoot 'dist\lepton_agent.exe' }
    if (-not (Test-Path $source)) { Write-Output 'FAILED: lepton_agent.exe not found, run build.ps1 first.'; exit 1 }
    $packaged = Join-Path $PSScriptRoot 'lepton\lepton_cam.exe'
    if (Test-Path $packaged) { $program = "`"$packaged`"" }
    else {
        $python = (Get-Command python -ErrorAction Stop).Source
        $program = "`"$python`" `"$(Join-Path $PSScriptRoot 'python\run_on_demand.py')`""
    }
    $arguments = "run -- $program --on-demand --model 3 --nativecam --no-preview"

    New-Item -ItemType Directory -Force $folder | Out-Null
    Stop-Agent
    Copy-Item $source $agent -Force
    Unblock-File $agent
    if (-not (Test-Path $runKey)) { New-Item $runKey -Force | Out-Null }
    Set-ItemProperty $runKey -Name $name -Value "`"$agent`" $arguments"
    Start-Process $agent -ArgumentList $arguments
    Start-Sleep 3
}
elseif ($Command -eq 'off') {
    Stop-Agent
    Remove-ItemProperty $runKey -Name $name -ErrorAction SilentlyContinue
    Remove-Item $agent -Force -ErrorAction SilentlyContinue
}

$entry = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).$name
$running = @(Get-Process lepton_agent -ErrorAction SilentlyContinue).Count -gt 0
"sign-in entry: $(if ($entry) { $entry } else { 'none' })"
"agent running: $running"
if ($Command -ne 'off' -and (Test-Path "$folder\agent.log")) { '--- last log lines'; Get-Content "$folder\agent.log" -Tail 6 }
if ($Command -eq 'on') { if ($entry -and $running) { 'AUTOSTART ON' } else { 'FAILED: the agent is not running'; exit 1 } }
if ($Command -eq 'off') { 'AUTOSTART OFF' }
