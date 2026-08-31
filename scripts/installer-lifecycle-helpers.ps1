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

function Get-UniqueLifecycleReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$ExpectedReceipt
    )

    $markerPrefix = 'EVOX2_LIFECYCLE_RECEIPT '
    $expectedMarker = "$markerPrefix$ExpectedReceipt"
    $markerCount = 0
    $expectedMatchCount = 0
    foreach ($line in $Lines) {
        $lineValue = [string]$line
        $searchIndex = 0
        while ($searchIndex -lt $lineValue.Length) {
            $markerIndex = $lineValue.IndexOf(
                $markerPrefix,
                $searchIndex,
                [StringComparison]::OrdinalIgnoreCase
            )
            if ($markerIndex -lt 0) {
                break
            }
            $markerCount += 1
            $searchIndex = $markerIndex + $markerPrefix.Length
        }

        $isExact = [string]::Equals(
            $lineValue,
            $expectedMarker,
            [StringComparison]::Ordinal
        )
        $hasBoundedSuffix = $lineValue.Length -gt $expectedMarker.Length -and
            $lineValue.EndsWith($expectedMarker, [StringComparison]::Ordinal) -and
            [char]::IsWhiteSpace($lineValue[$lineValue.Length - $expectedMarker.Length - 1])
        if ($isExact -or $hasBoundedSuffix) {
            $expectedMatchCount += 1
        }
    }

    if ($markerCount -ne 1 -or $expectedMatchCount -ne 1) {
        return $null
    }
    return $ExpectedReceipt
}

function Open-ClosedLifecycleLog {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [TimeSpan]$Timeout
    )

    $deadline = [DateTime]::UtcNow.Add($Timeout)
    do {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            try {
                return [IO.File]::Open(
                    $Path,
                    [IO.FileMode]::Open,
                    [IO.FileAccess]::Read,
                    [IO.FileShare]::None
                )
            } catch [IO.IOException] {
            }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Timed out waiting for lifecycle log closure: $Path"
}

function Wait-ForFileUnlock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [TimeSpan]$Timeout
    )

    $stream = Open-ClosedLifecycleLog -Path $Path -Timeout $Timeout
    try {
        return
    } finally {
        $stream.Dispose()
    }
}

function Copy-ClosedLifecycleLog {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDirectory,

        [Parameter(Mandatory = $true)]
        [TimeSpan]$Timeout
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        return $null
    }

    $destination = Join-Path $DestinationDirectory ([IO.Path]::GetFileName($Source))
    $sourceStream = $null
    $destinationStream = $null
    $destinationCreated = $false
    $copyError = $null
    try {
        $sourceStream = Open-ClosedLifecycleLog -Path $Source -Timeout $Timeout
        $destinationStream = [IO.File]::Open(
            $destination,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None
        )
        $destinationCreated = $true
        $sourceStream.CopyTo($destinationStream)
        $destinationStream.Flush()
    } catch {
        $copyError = $_
    } finally {
        if ($null -ne $destinationStream) {
            $destinationStream.Dispose()
        }
        if ($null -ne $sourceStream) {
            $sourceStream.Dispose()
        }
    }
    if ($null -ne $copyError) {
        if ($destinationCreated -and [IO.File]::Exists($destination)) {
            [IO.File]::Delete($destination)
        }
        throw $copyError
    }
    return $destination
}
