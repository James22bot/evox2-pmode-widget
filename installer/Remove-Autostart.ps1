#Requires -Version 5.1
#Requires -RunAsAdministrator

param([string]$LogPath)

$ErrorActionPreference = 'Stop'
trap {
    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        ($_ | Out-String) | Set-Content -LiteralPath $LogPath -Encoding UTF8
    }
    [Console]::Error.WriteLine(($_ | Out-String))
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$userSid = $identity.User.Value
$exactTask = "EVO-X2 P-MODE Widget $userSid"
$tasks = Get-ScheduledTask -ErrorAction Stop | Where-Object {
    $_.TaskName -eq 'EVO-X2 P-MODE Widget' -or
    $_.TaskName.StartsWith('EVO-X2 P-MODE Widget S-1-', [StringComparison]::Ordinal)
}
foreach ($task in $tasks) {
    Unregister-ScheduledTask -TaskName $task.TaskName -TaskPath $task.TaskPath -Confirm:$false -ErrorAction Stop
}
$remaining = Get-ScheduledTask -ErrorAction Stop | Where-Object {
    $_.TaskName -eq 'EVO-X2 P-MODE Widget' -or
    $_.TaskName.StartsWith('EVO-X2 P-MODE Widget S-1-', [StringComparison]::Ordinal)
}
if ($remaining) {
    throw 'One or more EVO-X2 P-MODE Widget tasks remain after removal.'
}
if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
    "AUTOSTART_REMOVAL=PASS $exactTask" | Set-Content -LiteralPath $LogPath -Encoding ASCII
}
Write-Host "AUTOSTART_REMOVAL=PASS $exactTask"