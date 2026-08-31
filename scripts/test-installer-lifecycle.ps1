#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SetupPath,
    [Parameter(Mandatory = $true)][string]$ExpectedAppPath,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.json$')]
    [string]$ReceiptName = 'installer-smoke.json'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

. (Join-Path $PSScriptRoot 'installer-lifecycle-helpers.ps1')

function Get-ProgramFiles64 {
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64
    )
    try {
        $currentVersion = $registry.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion')
        if ($null -eq $currentVersion) {
            throw '64-bit Windows CurrentVersion registry key is unavailable.'
        }
        try {
            $value = [string]$currentVersion.GetValue('ProgramFilesDir')
        } finally {
            $currentVersion.Dispose()
        }
    } finally {
        $registry.Dispose()
    }
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw '64-bit Program Files path is unavailable.'
    }
    return [IO.Path]::GetFullPath($value)
}

function Resolve-Sid([string]$Value) {
    if ($Value -match '^S-1-') {
        return $Value
    }
    return ([Security.Principal.NTAccount]$Value).Translate(
        [Security.Principal.SecurityIdentifier]
    ).Value
}

function Test-SamePath([string]$Left, [string]$Right) {
    return [string]::Equals(
        [IO.Path]::GetFullPath($Left),
        [IO.Path]::GetFullPath($Right),
        [StringComparison]::OrdinalIgnoreCase
    )
}

function Get-WidgetTasks {
    return @(
        Get-ScheduledTask -ErrorAction Stop | Where-Object {
            $_.TaskName -eq 'EVO-X2 P-MODE Widget' -or
            $_.TaskName.StartsWith('EVO-X2 P-MODE Widget S-1-', [StringComparison]::Ordinal)
        }
    )
}

function Get-WidgetProcesses {
    return @(
        Get-CimInstance Win32_Process -Filter "Name = 'evox2-pmode-overlay.exe'" -ErrorAction Stop
    )
}

function Get-WidgetProcessStartEvidence([string]$SourceIdentifier) {
    return @(
        Get-Event -SourceIdentifier $SourceIdentifier -ErrorAction SilentlyContinue | ForEach-Object {
            $trace = $_.SourceEventArgs.NewEvent
            [ordered]@{
                process_name = [string]$trace.ProcessName
                process_id = [uint32]$trace.ProcessID
                parent_process_id = [uint32]$trace.ParentProcessID
                time_created = [string]$trace.TIME_CREATED
            }
        }
    )
}

function Get-UninstallEntries {
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    return @(
        $roots | ForEach-Object {
            Get-ChildItem -LiteralPath $_ -ErrorAction SilentlyContinue |
                Get-ItemProperty -ErrorAction SilentlyContinue
        } | Where-Object { Test-IsWidgetUninstallEntry $_ }
    )
}

function Get-UninstallerPath($Entry, [string]$ExpectedDirectory) {
    $command = [string]$Entry.UninstallString
    $match = [regex]::Match($command, '^\s*"([^"]+)"\s*$')
    if (-not $match.Success) {
        throw 'Unexpected uninstall command.'
    }
    $path = [IO.Path]::GetFullPath($match.Groups[1].Value)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        -not (Test-SamePath ([IO.Path]::GetDirectoryName($path)) $ExpectedDirectory)) {
        throw 'Uninstaller is missing or outside the installation directory.'
    }
    return $path
}

function Wait-ForCleanRemoval(
    [string]$ApplicationDirectory,
    [string]$ShortcutPath,
    [TimeSpan]$Timeout
) {
    $deadline = [DateTime]::UtcNow.Add($Timeout)
    do {
        $tasks = @(Get-WidgetTasks)
        $entries = @(Get-UninstallEntries)
        $processes = @(Get-WidgetProcesses)
        $clean = -not (Test-Path -LiteralPath $ApplicationDirectory) -and
            -not (Test-Path -LiteralPath $ShortcutPath) -and
            $tasks.Count -eq 0 -and
            $entries.Count -eq 0 -and
            $processes.Count -eq 0
        if ($clean) {
            return
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Uninstall cleanup timed out: app=$([int](Test-Path -LiteralPath $ApplicationDirectory)) shortcut=$([int](Test-Path -LiteralPath $ShortcutPath)) tasks=$($tasks.Count) entries=$($entries.Count) processes=$($processes.Count)"
}

function Get-OwnedUninstallerCandidate([string]$ExpectedDirectory) {
    if ([string]::IsNullOrWhiteSpace($ExpectedDirectory) -or
        -not (Test-Path -LiteralPath $ExpectedDirectory -PathType Container)) {
        return $null
    }

    $candidates = @(
        [IO.Directory]::EnumerateFiles(
            $ExpectedDirectory,
            'unins*.exe',
            [IO.SearchOption]::TopDirectoryOnly
        )
    )
    if ($candidates.Count -gt 1) {
        throw "Multiple uninstaller candidates exist in the owned application directory: $($candidates.Count)"
    }
    if ($candidates.Count -eq 0) {
        return $null
    }

    $candidate = [IO.Path]::GetFullPath($candidates[0])
    if (-not (Test-SamePath ([IO.Path]::GetDirectoryName($candidate)) $ExpectedDirectory)) {
        throw 'Uninstaller candidate escaped the owned application directory.'
    }
    return $candidate
}

function Write-AtomicUtf8File([string]$Path, [string]$Content) {
    $directory = [IO.Path]::GetDirectoryName($Path)
    $temporaryPath = Join-Path $directory (
        ".$([IO.Path]::GetFileName($Path)).$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    )
    try {
        [IO.File]::WriteAllText(
            $temporaryPath,
            $Content,
            [Text.UTF8Encoding]::new($false)
        )
        [IO.File]::Move($temporaryPath, $Path)
    } finally {
        if ([IO.File]::Exists($temporaryPath)) {
            [IO.File]::Delete($temporaryPath)
        }
    }
}

function Format-ErrorRecord($Record) {
    if ($null -eq $Record) {
        return $null
    }
    return ($Record | Out-String).Trim()
}

function Write-SecondaryError([string]$Label, $Record) {
    if ($null -ne $Record) {
        [Console]::Error.WriteLine("${Label}: $(Format-ErrorRecord $Record)")
    }
}

# Establish the evidence destination first. Every subsequent failure is captured
# by the single reporting boundary below.
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$receiptPath = Join-Path $OutputDirectory $ReceiptName
if (Test-Path -LiteralPath $receiptPath) {
    throw "Refusing to replace pre-existing smoke receipt: $receiptPath"
}

$setupPathNormalized = $null
$expectedAppPathNormalized = $null
$programFiles = $null
$projectRoot = $null
$appDirectory = $null
$installedApp = $null
$shortcut = $null
$sid = $null
$taskName = $null
$installLog = $null
$uninstallLog = $null
$lifecycleLogNonce = $null
$processStartSourceIdentifier = $null
$processStartSubscription = $null
$processStartMonitorStarted = $false
$widgetProcessStartEvents = @()
$installStartedAtUtc = $null
$taskLastRunTimeUtc = $null
$taskRanSinceInstallStart = $null
$taskLastTaskResult = $null
$taskNumberOfMissedRuns = $null
$noStartEvidencePassed = $false
$scriptHash = $null
$setupHash = $null
$expectedAppHash = $null
$installedAppHash = $null
$installExitCode = $null
$widgetProcessCountAfterInstall = $null
$preflightClean = $false
$installationAttempted = $false
$uninstallRequested = $false
$ownedUninstaller = $null
$contractPassed = $false
$cleanupPassed = $false
$primaryError = $null
$primaryFailureCode = $null
$cleanupError = $null
$receiptError = $null
$evidenceErrors = @()
$copiedLogs = @()
$helperEvidence = @()
$taskEvidence = $null
$shortcutEvidence = $null
$uninstallEvidence = $null
$registrationReceipt = $null
$removalReceipt = $null
$preflightEvidence = [ordered]@{
    app_directory_exists = $null
    shortcut_exists = $null
    task_count = $null
    uninstall_entry_count = $null
    widget_process_count = $null
    clean = $false
}

try {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Installer lifecycle smoke requires an elevated Windows runner.'
    }

    $setupPathNormalized = [IO.Path]::GetFullPath($SetupPath)
    $expectedAppPathNormalized = [IO.Path]::GetFullPath($ExpectedAppPath)
    if (-not (Test-Path -LiteralPath $setupPathNormalized -PathType Leaf)) {
        throw 'Setup candidate is missing.'
    }
    if (-not (Test-Path -LiteralPath $expectedAppPathNormalized -PathType Leaf)) {
        throw 'Expected application candidate is missing.'
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedVersion)) {
        throw 'Expected version is empty.'
    }

    $programFiles = Get-ProgramFiles64
    $projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $appDirectory = [IO.Path]::GetFullPath((Join-Path $programFiles 'EVO-X2 P-MODE Overlay'))
    $installedApp = [IO.Path]::GetFullPath((Join-Path $appDirectory 'evox2-pmode-overlay.exe'))
    $shortcut = [IO.Path]::GetFullPath((Join-Path (
        [Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)
    ) 'EVO-X2 P-MODE Widget.lnk'))
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $taskName = "EVO-X2 P-MODE Widget $sid"
    $temporaryDirectory = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
        [IO.Path]::GetTempPath()
    } else {
        [IO.Path]::GetFullPath($env:RUNNER_TEMP)
    }

    $scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $setupHash = (Get-FileHash -LiteralPath $setupPathNormalized -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedAppHash = (Get-FileHash -LiteralPath $expectedAppPathNormalized -Algorithm SHA256).Hash.ToLowerInvariant()

    $lifecycleLogNonce = [Guid]::NewGuid().ToString('N')
    $processStartSourceIdentifier = "evox2-installer-smoke-$PID-$lifecycleLogNonce"
    Register-CimIndicationEvent `
        -Namespace 'root/cimv2' `
        -Query "SELECT * FROM Win32_ProcessStartTrace WHERE ProcessName = 'evox2-pmode-overlay.exe'" `
        -SourceIdentifier $processStartSourceIdentifier | Out-Null
    $processStartSubscription = Get-EventSubscriber `
        -SourceIdentifier $processStartSourceIdentifier `
        -ErrorAction Stop
    if ($null -eq $processStartSubscription) {
        throw 'Widget process-start monitor could not be registered.'
    }
    $processStartMonitorStarted = $true

    $preflightTasks = @(Get-WidgetTasks)
    $preflightEntries = @(Get-UninstallEntries)
    $preflightProcesses = @(Get-WidgetProcesses)
    $preflightEvidence['app_directory_exists'] = Test-Path -LiteralPath $appDirectory
    $preflightEvidence['shortcut_exists'] = Test-Path -LiteralPath $shortcut
    $preflightEvidence['task_count'] = $preflightTasks.Count
    $preflightEvidence['uninstall_entry_count'] = $preflightEntries.Count
    $preflightEvidence['widget_process_count'] = $preflightProcesses.Count
    if ($preflightEvidence['app_directory_exists']) {
        $primaryFailureCode = 'preflight_app_directory_exists'
        throw 'Preflight is not clean: application directory already exists.'
    }
    if ($preflightEvidence['shortcut_exists']) {
        $primaryFailureCode = 'preflight_shortcut_exists'
        throw 'Preflight is not clean: Start-menu shortcut already exists.'
    }
    if ($preflightTasks.Count -ne 0 -or $preflightEntries.Count -ne 0 -or $preflightProcesses.Count -ne 0) {
        $primaryFailureCode = 'preflight_resource_state_present'
        throw "Preflight is not clean: tasks=$($preflightTasks.Count) entries=$($preflightEntries.Count) processes=$($preflightProcesses.Count)"
    }
    $preflightClean = $true
    $preflightEvidence['clean'] = $true

    $installLog = Join-Path $temporaryDirectory "evox2-setup-install-$lifecycleLogNonce.log"
    $uninstallLog = Join-Path $temporaryDirectory "evox2-setup-uninstall-$lifecycleLogNonce.log"
    foreach ($path in @($installLog, $uninstallLog)) {
        if (Test-Path -LiteralPath $path) {
            throw "Unique lifecycle log path already exists: $path"
        }
    }

    $installStartedAtUtc = [DateTime]::UtcNow
    $installationAttempted = $true
    $install = Start-Process -FilePath $setupPathNormalized -ArgumentList @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        "/LIFECYCLELOGNONCE=$lifecycleLogNonce",
        "/LOG=`"$installLog`""
    ) -Wait -PassThru
    $installExitCode = $install.ExitCode
    $ownedUninstaller = Get-OwnedUninstallerCandidate $appDirectory
    if ($installExitCode -ne 0) {
        throw "Silent install failed: $installExitCode"
    }

    if (-not (Test-Path -LiteralPath $installedApp -PathType Leaf)) {
        throw 'Installed application is missing.'
    }
    $installedAppHash = (Get-FileHash -LiteralPath $installedApp -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($installedAppHash -ne $expectedAppHash) {
        throw "Installed application hash mismatch: expected=$expectedAppHash actual=$installedAppHash"
    }
    foreach ($scriptName in @('Register-Autostart.ps1', 'Remove-Autostart.ps1')) {
        $sourceScript = Join-Path (Join-Path $projectRoot 'installer') $scriptName
        $installedScript = Join-Path $appDirectory $scriptName
        if (-not (Test-Path -LiteralPath $installedScript -PathType Leaf)) {
            throw "Installed helper is missing: $scriptName"
        }
        $sourceHash = (Get-FileHash -LiteralPath $sourceScript -Algorithm SHA256).Hash.ToLowerInvariant()
        $installedHash = (Get-FileHash -LiteralPath $installedScript -Algorithm SHA256).Hash.ToLowerInvariant()
        $helperEvidence += [ordered]@{
            name = $scriptName
            source_sha256 = $sourceHash
            installed_sha256 = $installedHash
        }
        if ($installedHash -ne $sourceHash) {
            throw "Installed helper hash mismatch: $scriptName"
        }
    }
    $widgetProcesses = @(Get-WidgetProcesses)
    $widgetProcessCountAfterInstall = $widgetProcesses.Count
    if ($widgetProcessCountAfterInstall -ne 0) {
        throw 'Silent installation unexpectedly started the widget.'
    }

    $tasks = @(Get-WidgetTasks)
    if ($tasks.Count -ne 1) {
        throw "Expected exactly one widget task, found $($tasks.Count)."
    }
    $task = $tasks[0]
    if ($task.Triggers.Count -ne 1 -or $task.Actions.Count -ne 1) {
        throw 'Scheduled task must have exactly one trigger and one action.'
    }
    $principalSid = Resolve-Sid ([string]$task.Principal.UserId)
    $triggerSid = Resolve-Sid ([string]$task.Triggers[0].UserId)
    $argumentsEmpty = [string]::IsNullOrEmpty([string]$task.Actions[0].Arguments)
    if ($task.TaskName -ne $taskName -or
        $task.TaskPath -ne '\' -or
        -not (Test-SamePath ([string]$task.Actions[0].Execute) $installedApp) -or
        -not (Test-SamePath ([string]$task.Actions[0].WorkingDirectory) $appDirectory) -or
        -not $argumentsEmpty -or
        $task.Principal.RunLevel.ToString() -ne 'Highest' -or
        $task.Principal.LogonType.ToString() -ne 'Interactive' -or
        $principalSid -ne $sid -or
        $task.Triggers[0].Enabled -ne $true -or
        $triggerSid -ne $sid -or
        $task.Settings.Enabled -ne $true -or
        $task.Settings.StartWhenAvailable -ne $true -or
        $task.Settings.MultipleInstances.ToString() -ne 'IgnoreNew' -or
        $task.Settings.DisallowStartIfOnBatteries -ne $false -or
        $task.Settings.StopIfGoingOnBatteries -ne $false -or
        $task.State.ToString() -ne 'Ready') {
        throw 'Scheduled-task lifecycle contract mismatch.'
    }
    $taskInfo = Get-ScheduledTaskInfo -InputObject $task -ErrorAction Stop
    $taskLastRunTimeUtc = ([DateTime]$taskInfo.LastRunTime).ToUniversalTime()
    $taskLastTaskResult = [long]$taskInfo.LastTaskResult
    $taskNumberOfMissedRuns = [long]$taskInfo.NumberOfMissedRuns
    $taskRanSinceInstallStart = $taskLastRunTimeUtc -ge $installStartedAtUtc
    if ($taskRanSinceInstallStart) {
        throw "Scheduled task ran after smoke installation began: $($taskLastRunTimeUtc.ToString('o'))"
    }
    $expectedRegistrationReceipt = "AUTOSTART_TASK=PASS $taskName"
    $registrationReceipt = Get-UniqueLifecycleReceipt `
        -Lines @(Get-Content -LiteralPath $installLog -ErrorAction Stop) `
        -ExpectedReceipt $expectedRegistrationReceipt
    if ($null -eq $registrationReceipt) {
        throw 'Autostart registration receipt mismatch.'
    }
    $taskEvidence = [ordered]@{
        name = $task.TaskName
        path = $task.TaskPath
        state = $task.State.ToString()
        trigger_count = $task.Triggers.Count
        action_count = $task.Actions.Count
        principal_sid = $principalSid
        run_level = $task.Principal.RunLevel.ToString()
        logon_type = $task.Principal.LogonType.ToString()
        trigger_sid = $triggerSid
        trigger_enabled = [bool]$task.Triggers[0].Enabled
        execute = [string]$task.Actions[0].Execute
        working_directory = [string]$task.Actions[0].WorkingDirectory
        arguments_empty = $argumentsEmpty
        start_when_available = [bool]$task.Settings.StartWhenAvailable
        multiple_instances = $task.Settings.MultipleInstances.ToString()
        allow_start_on_batteries = -not [bool]$task.Settings.DisallowStartIfOnBatteries
        dont_stop_on_batteries = -not [bool]$task.Settings.StopIfGoingOnBatteries
        last_run_time_utc = $taskLastRunTimeUtc.ToString('o')
        last_task_result = $taskLastTaskResult
        number_of_missed_runs = $taskNumberOfMissedRuns
        ran_since_install_start = $taskRanSinceInstallStart
    }

    if (-not (Test-Path -LiteralPath $shortcut -PathType Leaf)) {
        throw 'Start-menu shortcut is missing.'
    }
    $shell = $null
    $link = $null
    try {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut($shortcut)
        $shortcutTarget = [string]$link.TargetPath
        $shortcutWorkingDirectory = [string]$link.WorkingDirectory
        $shortcutArguments = [string]$link.Arguments
    } finally {
        if ($null -ne $link) {
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($link)
        }
        if ($null -ne $shell) {
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
        }
    }
    if (-not (Test-SamePath $shortcutTarget $installedApp) -or
        -not (Test-SamePath $shortcutWorkingDirectory $appDirectory) -or
        -not [string]::IsNullOrEmpty($shortcutArguments)) {
        throw 'Start-menu shortcut contract mismatch.'
    }
    $shortcutEvidence = [ordered]@{
        path = $shortcut
        target = $shortcutTarget
        working_directory = $shortcutWorkingDirectory
        arguments_empty = [string]::IsNullOrEmpty($shortcutArguments)
    }

    $entries = @(Get-UninstallEntries)
    if ($entries.Count -ne 1) {
        throw "Expected exactly one uninstall registration, found $($entries.Count)."
    }
    $entry = $entries[0]
    if ([string]$entry.DisplayVersion -ne $ExpectedVersion -or
        [string]$entry.Publisher -ne 'Andreas Ruhl') {
        throw 'Uninstall registration metadata mismatch.'
    }
    $uninstaller = Get-UninstallerPath $entry $appDirectory
    $ownedUninstaller = $uninstaller
    $uninstallEvidence = [ordered]@{
        display_name = [string]$entry.DisplayName
        display_version = [string]$entry.DisplayVersion
        publisher = [string]$entry.Publisher
        uninstaller = $uninstaller
        uninstaller_sha256 = (Get-FileHash -LiteralPath $uninstaller -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $contractPassed = $true

    $uninstallRequested = $true
    Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        "/LIFECYCLELOGNONCE=$lifecycleLogNonce",
        "/LOG=`"$uninstallLog`""
    ) -PassThru | Out-Null
    Wait-ForCleanRemoval $appDirectory $shortcut ([TimeSpan]::FromSeconds(90))
    Wait-ForFileUnlock $uninstallLog ([TimeSpan]::FromSeconds(10))
    $expectedRemovalReceipt = "AUTOSTART_REMOVAL=PASS $taskName"
    $removalReceipt = Get-UniqueLifecycleReceipt `
        -Lines @(Get-Content -LiteralPath $uninstallLog -ErrorAction Stop) `
        -ExpectedReceipt $expectedRemovalReceipt
    if ($null -eq $removalReceipt) {
        throw 'Autostart removal receipt mismatch.'
    }
    $cleanupPassed = $true
} catch {
    if ($null -eq $primaryFailureCode) {
        $primaryFailureCode = 'unexpected_failure'
    }
    $primaryError = $_
} finally {
    # Cleanup is allowed only after a clean preflight and this invocation's
    # installation attempt. Dirty pre-existing state is never modified.
    if ($preflightClean -and $installationAttempted -and -not $cleanupPassed) {
        try {
            if (-not $uninstallRequested -and $null -eq $ownedUninstaller) {
                $ownedUninstaller = Get-OwnedUninstallerCandidate $appDirectory
            }
            if (-not $uninstallRequested -and $null -ne $ownedUninstaller) {
                $uninstallRequested = $true
                Start-Process -FilePath $ownedUninstaller -ArgumentList @(
                    '/VERYSILENT',
                    '/SUPPRESSMSGBOXES',
                    '/NORESTART',
                    "/LIFECYCLELOGNONCE=$lifecycleLogNonce",
                    "/LOG=`"$uninstallLog`""
                ) -PassThru | Out-Null
            }

            $residualStateExists = (Test-Path -LiteralPath $appDirectory) -or
                (Test-Path -LiteralPath $shortcut) -or
                @(Get-WidgetTasks).Count -ne 0 -or
                @(Get-UninstallEntries).Count -ne 0 -or
                @(Get-WidgetProcesses).Count -ne 0
            if ($residualStateExists -and -not $uninstallRequested) {
                throw 'Owned installation state remains, but no unique owned uninstaller is available.'
            }
            Wait-ForCleanRemoval $appDirectory $shortcut ([TimeSpan]::FromSeconds(90))
            $cleanupPassed = $true
        } catch {
            $cleanupError = $_
        }
    }

    if ($processStartMonitorStarted) {
        try {
            Start-Sleep -Milliseconds 750
            $widgetProcessStartEvents = @(
                Get-WidgetProcessStartEvidence $processStartSourceIdentifier
            )
        } catch {
            $evidenceErrors += $_
        } finally {
            try {
                Unregister-Event -SourceIdentifier $processStartSourceIdentifier -ErrorAction Stop
            } catch {
                $evidenceErrors += $_
            }
            foreach ($queuedEvent in @(
                Get-Event -SourceIdentifier $processStartSourceIdentifier -ErrorAction SilentlyContinue
            )) {
                Remove-Event -EventIdentifier $queuedEvent.EventIdentifier -ErrorAction SilentlyContinue
            }
        }
    }
    $noStartEvidencePassed = $processStartMonitorStarted -and
        $widgetProcessStartEvents.Count -eq 0 -and
        $widgetProcessCountAfterInstall -eq 0 -and
        $taskRanSinceInstallStart -eq $false
    if ($null -eq $primaryError -and -not $noStartEvidencePassed) {
        try {
            throw "No-start evidence failed: monitor=$processStartMonitorStarted process_events=$($widgetProcessStartEvents.Count) current_processes=$widgetProcessCountAfterInstall task_ran=$taskRanSinceInstallStart"
        } catch {
            $evidenceErrors += $_
        }
    }

    foreach ($path in @($installLog, $uninstallLog)) {
        if (-not [string]::IsNullOrWhiteSpace($path)) {
            try {
                if (Test-Path -LiteralPath $path -PathType Leaf) {
                    $copiedPath = Copy-ClosedLifecycleLog `
                        -Source $path `
                        -DestinationDirectory $OutputDirectory `
                        -Timeout ([TimeSpan]::FromSeconds(10))
                    if ($null -ne $copiedPath) {
                        $copiedLogs += [IO.Path]::GetFileName($copiedPath)
                    }
                }
            } catch {
                $evidenceErrors += $_
            }
        }
    }

    $status = if ($contractPassed -and $cleanupPassed -and $noStartEvidencePassed -and $null -eq $primaryError -and
        $null -eq $cleanupError -and $evidenceErrors.Count -eq 0) {
        'PASS'
    } else {
        'FAIL'
    }
    $receipt = [ordered]@{
        schema = 'evox2-installer-smoke-v4'
        status = $status
        version = $ExpectedVersion
        producer = [ordered]@{
            script_sha256 = $scriptHash
            github_sha = $env:GITHUB_SHA
            github_ref = $env:GITHUB_REF
            github_run_id = $env:GITHUB_RUN_ID
            github_run_attempt = $env:GITHUB_RUN_ATTEMPT
            github_workflow = $env:GITHUB_WORKFLOW
            runner_os = $env:RUNNER_OS
            image_os = $env:ImageOS
            image_version = $env:ImageVersion
        }
        setup_path = $setupPathNormalized
        expected_app_path = $expectedAppPathNormalized
        setup_sha256 = $setupHash
        expected_app_sha256 = $expectedAppHash
        installed_app_sha256 = $installedAppHash
        preflight = $preflightEvidence
        installation_attempted = $installationAttempted
        lifecycle_owned = ($preflightClean -and $installationAttempted)
        silent_install_exit_code = $installExitCode
        widget_process_count_after_install = $widgetProcessCountAfterInstall
        task_start_behavior = [ordered]@{
            observed_no_start = $noStartEvidencePassed
            process_start_monitor_started = $processStartMonitorStarted
            process_start_event_count = $widgetProcessStartEvents.Count
            process_start_events = @($widgetProcessStartEvents)
            install_started_at_utc = if ($null -eq $installStartedAtUtc) { $null } else { $installStartedAtUtc.ToString('o') }
            task_last_run_time_utc = if ($null -eq $taskLastRunTimeUtc) { $null } else { $taskLastRunTimeUtc.ToString('o') }
            task_last_run_after_install_start = $taskRanSinceInstallStart
            task_last_task_result = $taskLastTaskResult
            task_number_of_missed_runs = $taskNumberOfMissedRuns
            contract = 'A Win32_ProcessStartTrace subscription covers the owned install lifecycle, the widget process count is sampled after install, and Task Scheduler LastRunTime must predate installation.'
        }
        helper_hashes = $helperEvidence
        task_contract = $taskEvidence
        registration_receipt = $registrationReceipt
        shortcut_contract = $shortcutEvidence
        uninstall_registration = $uninstallEvidence
        removal_receipt = $removalReceipt
        contract_passed = $contractPassed
        uninstall_requested = $uninstallRequested
        cleanup_passed = $cleanupPassed
        copied_logs = $copiedLogs
        primary_failure_code = $primaryFailureCode
        primary_failure = Format-ErrorRecord $primaryError
        cleanup_failure = Format-ErrorRecord $cleanupError
        evidence_failures = @($evidenceErrors | ForEach-Object { Format-ErrorRecord $_ })
        recorded_at_utc = [DateTime]::UtcNow.ToString('o')
    }
    try {
        $json = $receipt | ConvertTo-Json -Depth 10
        Write-AtomicUtf8File $receiptPath ($json + [Environment]::NewLine)
    } catch {
        $receiptError = $_
    }
}

Write-SecondaryError 'Cleanup failure' $cleanupError
foreach ($errorRecord in $evidenceErrors) {
    Write-SecondaryError 'Evidence collection failure' $errorRecord
}
Write-SecondaryError 'Receipt write failure' $receiptError

if ($null -ne $primaryError) {
    $PSCmdlet.ThrowTerminatingError($primaryError)
}
if ($null -ne $cleanupError) {
    $PSCmdlet.ThrowTerminatingError($cleanupError)
}
if ($evidenceErrors.Count -ne 0) {
    $PSCmdlet.ThrowTerminatingError($evidenceErrors[0])
}
if ($null -ne $receiptError) {
    $PSCmdlet.ThrowTerminatingError($receiptError)
}
if (-not $contractPassed -or -not $cleanupPassed -or -not $noStartEvidencePassed) {
    throw 'Installer lifecycle smoke did not complete its required contracts.'
}

Write-Host "INSTALLER_SMOKE=PASS receipt=$receiptPath setup_sha256=$setupHash app_sha256=$installedAppHash"
