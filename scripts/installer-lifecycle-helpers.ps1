#Requires -Version 5.1

Set-StrictMode -Version Latest

function Test-IsWidgetUninstallEntry {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, ValueFromPipeline = $true)]
        $Entry
    )

    process {
        if ($null -eq $Entry) {
            return $false
        }

        $displayName = $Entry.PSObject.Properties['DisplayName']
        return $null -ne $displayName -and [string]::Equals(
            [string]$displayName.Value,
            'EVO-X2 P-MODE Widget',
            [StringComparison]::OrdinalIgnoreCase
        )
    }
}
