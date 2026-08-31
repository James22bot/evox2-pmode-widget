#Requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot '..\scripts\installer-lifecycle-helpers.ps1')

$missingDisplayName = [pscustomobject]@{
    RegistryPath = 'HKLM:\Software\Example\MissingDisplayName'
}
$otherProduct = [pscustomobject]@{
    DisplayName = 'Another Product'
    Marker = 'other'
}
$widget = [pscustomobject]@{
    DisplayName = 'EVO-X2 P-MODE Widget'
    Marker = 'widget'
}

$matches = @(
    @($missingDisplayName, $otherProduct, $widget) |
        Where-Object { Test-IsWidgetUninstallEntry $_ }
)
if ($matches.Count -ne 1 -or $matches[0].Marker -ne 'widget') {
    throw 'Uninstall-entry filtering must ignore missing properties and select only the widget.'
}

$expectedReceipt = 'AUTOSTART_TASK=PASS EVO-X2 P-MODE Widget S-1-5-21-1'
$receipt = Get-UniqueLifecycleReceipt -Lines @(
    '2026-08-30 23:23:38.000   unrelated message',
    "2026-08-30 23:23:38.001   EVOX2_LIFECYCLE_RECEIPT $expectedReceipt",
    '2026-08-30 23:23:38.002   another unrelated message'
) -ExpectedReceipt $expectedReceipt
if ($receipt -ne $expectedReceipt) {
    throw 'Lifecycle receipt parsing must accept one exact, boundary-delimited marker.'
}

$duplicateReceipt = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $expectedReceipt",
    "2026-08-30 23:23:38.002   EVOX2_LIFECYCLE_RECEIPT $expectedReceipt"
) -ExpectedReceipt $expectedReceipt
if ($null -ne $duplicateReceipt) {
    throw 'Lifecycle receipt parsing must reject duplicate markers.'
}

$sameLineDuplicate = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $expectedReceipt EVOX2_LIFECYCLE_RECEIPT $expectedReceipt"
) -ExpectedReceipt $expectedReceipt
if ($null -ne $sameLineDuplicate) {
    throw 'Lifecycle receipt parsing must reject two markers on one line.'
}

$unexpectedMarker = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $expectedReceipt",
    'EVOX2_LIFECYCLE_RECEIPT AUTOSTART_REMOVAL=PASS unexpected'
) -ExpectedReceipt $expectedReceipt
if ($null -ne $unexpectedMarker) {
    throw 'Lifecycle receipt parsing must reject every unexpected lifecycle marker.'
}

$embeddedMarker = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $expectedReceipt",
    "not-a-boundaryEVOX2_LIFECYCLE_RECEIPT $expectedReceipt"
) -ExpectedReceipt $expectedReceipt
if ($null -ne $embeddedMarker) {
    throw 'Lifecycle receipt parsing must reject embedded lifecycle markers.'
}

$trailingContent = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $expectedReceipt trailing"
) -ExpectedReceipt $expectedReceipt
if ($null -ne $trailingContent) {
    throw 'Lifecycle receipt parsing must reject trailing content.'
}

$caseVariation = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $($expectedReceipt.ToLowerInvariant())"
) -ExpectedReceipt $expectedReceipt
if ($null -ne $caseVariation) {
    throw 'Lifecycle receipt parsing must remain ordinal and case-sensitive.'
}

$caseVariantMarker = Get-UniqueLifecycleReceipt -Lines @(
    "EVOX2_LIFECYCLE_RECEIPT $expectedReceipt",
    'evox2_lifecycle_receipt unrelated'
) -ExpectedReceipt $expectedReceipt
if ($null -ne $caseVariantMarker) {
    throw 'Lifecycle receipt parsing must reject case-variant unexpected markers.'
}

$emptyReceipt = Get-UniqueLifecycleReceipt -Lines @() -ExpectedReceipt $expectedReceipt
if ($null -ne $emptyReceipt) {
    throw 'Lifecycle receipt parsing must reject empty evidence.'
}

$copyTestRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "evox2-lifecycle-copy-test-$([Guid]::NewGuid().ToString('N'))"
)
$copyDestination = Join-Path $copyTestRoot 'evidence'
$lockedSource = Join-Path $copyTestRoot 'evox2-setup-uninstall.log'
$lockedDestination = Join-Path $copyDestination ([IO.Path]::GetFileName($lockedSource))
$writer = $null
try {
    [IO.Directory]::CreateDirectory($copyDestination) | Out-Null
    $writer = [IO.File]::Open(
        $lockedSource,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::ReadWrite
    )
    $payload = [Text.Encoding]::ASCII.GetBytes('partial-uninstall-log')
    $writer.Write($payload, 0, $payload.Length)
    $writer.Flush()

    $copyFailure = $null
    try {
        Copy-ClosedLifecycleLog `
            -Source $lockedSource `
            -DestinationDirectory $copyDestination `
            -Timeout ([TimeSpan]::FromMilliseconds(250)) | Out-Null
    } catch {
        $copyFailure = $_
    }
    if ($null -eq $copyFailure -or
        $copyFailure.Exception.Message -notlike 'Timed out waiting for lifecycle log closure:*') {
        throw 'Locked lifecycle logs must fail at the closure gate.'
    }
    if (Test-Path -LiteralPath $lockedDestination) {
        throw 'A lifecycle log must not be copied after its closure gate fails.'
    }

    $writer.Dispose()
    $writer = $null
    $copiedPath = Copy-ClosedLifecycleLog `
        -Source $lockedSource `
        -DestinationDirectory $copyDestination `
        -Timeout ([TimeSpan]::FromSeconds(2))
    if (-not [string]::Equals(
            [IO.Path]::GetFullPath($copiedPath),
            [IO.Path]::GetFullPath($lockedDestination),
            [StringComparison]::OrdinalIgnoreCase
        )) {
        throw 'A closed lifecycle log must be copied to its expected evidence path.'
    }
    $copiedPayload = [IO.File]::ReadAllBytes($lockedDestination)
    if ([Convert]::ToBase64String($copiedPayload) -ne [Convert]::ToBase64String($payload)) {
        throw 'Lifecycle log evidence must preserve the source bytes exactly.'
    }
} finally {
    if ($null -ne $writer) {
        $writer.Dispose()
    }
    if ([IO.Directory]::Exists($copyTestRoot)) {
        [IO.Directory]::Delete($copyTestRoot, $true)
    }
}

Write-Host 'INSTALLER_LIFECYCLE_UNIT=PASS'
