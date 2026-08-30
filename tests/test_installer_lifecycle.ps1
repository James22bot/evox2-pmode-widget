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

Write-Host 'INSTALLER_LIFECYCLE_UNIT=PASS'
