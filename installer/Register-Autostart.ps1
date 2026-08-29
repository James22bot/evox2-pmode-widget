#Requires -Version 5.1
#Requires -RunAsAdministrator

param(
    [Parameter(Mandatory = $true)][string]$AppPath,
    [Parameter(Mandatory = $true)][string]$WorkingDirectory,
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'
trap {
    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        ($_ | Out-String) | Set-Content -LiteralPath $LogPath -Encoding UTF8
    }
    [Console]::Error.WriteLine(($_ | Out-String))
    exit 1
}

$programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
$expectedDirectory = [IO.Path]::GetFullPath((Join-Path $programFiles 'EVO-X2 P-MODE Overlay'))
$expectedApp = [IO.Path]::GetFullPath((Join-Path $expectedDirectory 'evox2-pmode-overlay.exe'))
$AppPath = [IO.Path]::GetFullPath($AppPath)
$WorkingDirectory = [IO.Path]::GetFullPath($WorkingDirectory)
if (-not $AppPath.Equals($expectedApp, [StringComparison]::OrdinalIgnoreCase) -or
    -not $WorkingDirectory.Equals($expectedDirectory, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unexpected installation path.'
}
$directoryItem = Get-Item -LiteralPath $WorkingDirectory -Force
if (-not $directoryItem.PSIsContainer -or
    ($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    -not (Test-Path -LiteralPath $AppPath -PathType Leaf)) {
    throw 'Unsafe or incomplete installation directory.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$userSid = $identity.User.Value
$taskName = "EVO-X2 P-MODE Widget $userSid"
$legacyTaskName = 'EVO-X2 P-MODE Widget'
if (Get-ScheduledTask -TaskName $legacyTaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $legacyTaskName -Confirm:$false -ErrorAction Stop
}

$taskTouched = $false
try {
    $action = New-ScheduledTaskAction -Execute $AppPath -WorkingDirectory $WorkingDirectory
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $userSid
    $principal = New-ScheduledTaskPrincipal -UserId $userSid -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -MultipleInstances IgnoreNew
    $definition = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings
    $taskTouched = $true
    Register-ScheduledTask -TaskName $taskName -InputObject $definition -Force | Out-Null

    function Resolve-Sid([string]$Value) {
        if ($Value -match '^S-1-') { return $Value }
        return ([Security.Principal.NTAccount]$Value).Translate([Security.Principal.SecurityIdentifier]).Value
    }

    $registered = Get-ScheduledTask -TaskName $taskName -ErrorAction Stop
    $principalSid = Resolve-Sid $registered.Principal.UserId
    if ($registered.Triggers.Count -ne 1) { throw 'Unexpected trigger count.' }
    $triggerSid = Resolve-Sid $registered.Triggers[0].UserId
    if ($registered.TaskName -ne $taskName -or
        $registered.TaskPath -ne '\' -or
        $registered.Actions.Count -ne 1 -or
        -not [string]::Equals($registered.Actions[0].Execute, $AppPath, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals($registered.Actions[0].WorkingDirectory, $WorkingDirectory, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::IsNullOrEmpty($registered.Actions[0].Arguments) -or
        $registered.Principal.RunLevel.ToString() -ne 'Highest' -or
        $registered.Principal.LogonType.ToString() -ne 'Interactive' -or
        $principalSid -ne $userSid -or
        $registered.Triggers[0].Enabled -ne $true -or
        $triggerSid -ne $userSid -or
        $registered.Settings.Enabled -ne $true -or
        $registered.Settings.StartWhenAvailable -ne $true -or
        $registered.Settings.MultipleInstances.ToString() -ne 'IgnoreNew') {
        throw 'Scheduled-task readback failed.'
    }
} catch {
    if ($taskTouched) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
        if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
            throw 'Scheduled-task cleanup failed after registration error.'
        }
    }
    throw
}

if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
    "AUTOSTART_TASK=PASS $taskName" | Set-Content -LiteralPath $LogPath -Encoding UTF8
}
Write-Host "AUTOSTART_TASK=PASS $taskName"