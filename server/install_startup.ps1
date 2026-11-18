$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pyScript  = Join-Path $scriptDir "quota_server.py"

$pythonw = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $pythonw) {
  $py = (Get-Command python.exe -ErrorAction SilentlyContinue).Source
  if (-not $py) { throw "python.exe / pythonw.exe tidak ditemukan di PATH" }
  $pythonw = $py
}

# 1. Startup folder (.vbs silent tanpa window)
$startupDir = [Environment]::GetFolderPath("Startup")
$vbsPath    = Join-Path $startupDir "AntigravityQuotaServer.vbs"
$vbsContent = @"
Set WshShell = CreateObject("WScript.Shell")
WshShell.Run """$pythonw"" ""$pyScript""", 0, False
"@
Set-Content -Path $vbsPath -Value $vbsContent -Encoding ASCII
Write-Host "Startup VBS dibuat di: $vbsPath"

# 2. Scheduled Task (jalan otomatis saat login & tetap hidup di background)
$taskName = "AntigravityQuotaServer"
$action   = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$pyScript`"" -WorkingDirectory $scriptDir
$trigger  = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host "Scheduled Task '$taskName' terdaftar dan dijalankan (port 8787)."
