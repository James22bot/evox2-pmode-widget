#Requires -Version 5.1

param(
    [Parameter(Mandatory = $true)][string]$Iscc
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $root
$version = (Get-Content -LiteralPath 'VERSION' -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+-beta\.\d+$') {
    throw "Unexpected release version: $version"
}
$iss = Get-Content -LiteralPath 'installer\widget.iss' -Raw
if ($iss -notmatch ('#define MyAppVersion "' + [regex]::Escape($version) + '"')) {
    throw 'VERSION and installer/widget.iss disagree.'
}
$widgetRunEntry = [regex]::Match(
    $iss,
    '(?m)^Filename:\s*"\{app\}\\\{#MyAppExeName\}";.*?Flags:\s*([^\r\n]+)$'
)
if (-not $widgetRunEntry.Success) {
    throw 'Widget post-install launch entry is missing.'
}
$widgetRunFlags = @($widgetRunEntry.Groups[1].Value -split '\s+' | Where-Object { $_ })
if ($widgetRunFlags -notcontains 'postinstall' -or $widgetRunFlags -notcontains 'runascurrentuser') {
    throw 'Widget post-install launch must explicitly retain the elevated Setup token.'
}
$dist = Join-Path $root 'dist'
if (Test-Path -LiteralPath $dist) {
    Remove-Item -LiteralPath $dist -Recurse -Force
}
[IO.Directory]::CreateDirectory($dist) | Out-Null

& $Iscc (Join-Path $root 'installer\widget.iss')
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed with exit code $LASTEXITCODE"
}

$portableName = "EVO-X2-PMode-Widget-v$version"
$portable = Join-Path $dist $portableName
[IO.Directory]::CreateDirectory($portable) | Out-Null
$copies = @(
    @{ Source = 'build\windows\evox2-pmode-overlay.exe'; Name = 'evox2-pmode-overlay.exe' },
    @{ Source = 'third_party\PawnIO.Modules-0.1.6\LpcACPIEC.bin'; Name = 'LpcACPIEC.bin' },
    @{ Source = 'README.md'; Name = 'README.md' },
    @{ Source = 'LICENSE'; Name = 'LICENSE.txt' },
    @{ Source = 'SECURITY.md'; Name = 'SECURITY.md' },
    @{ Source = 'THIRD_PARTY_NOTICES.md'; Name = 'THIRD_PARTY_NOTICES.md' },
    @{ Source = 'third_party\PawnIO.Modules-0.1.6\COPYING'; Name = 'COPYING-PawnIO-Modules-LGPL-2.1.txt' },
    @{ Source = 'third_party\PawnIO.Modules-0.1.6\LpcACPIEC.p'; Name = 'LpcACPIEC.p' }
)
foreach ($item in $copies) {
    Copy-Item -LiteralPath (Join-Path $root $item.Source) -Destination (Join-Path $portable $item.Name)
}

$portableZip = Join-Path $dist "$portableName-windows-x64.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory(
    $portable,
    $portableZip,
    [IO.Compression.CompressionLevel]::Optimal,
    $true
)
Remove-Item -LiteralPath $portable -Recurse -Force

$setup = Join-Path $dist "EVO-X2-PMode-Widget-Setup-v$version.exe"
if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) {
    throw "Installer output missing: $setup"
}

$artifacts = @($setup, $portableZip) | Sort-Object
$manifest = Join-Path $dist 'SHA256SUMS.txt'
$lines = foreach ($path in $artifacts) {
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($path))"
}
[IO.File]::WriteAllLines($manifest, $lines, [Text.UTF8Encoding]::new($false))
foreach ($line in Get-Content -LiteralPath $manifest) {
    $parts = $line -split '  ', 2
    $actual = (Get-FileHash -LiteralPath (Join-Path $dist $parts[1]) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $parts[0]) {
        throw "Release manifest readback mismatch: $($parts[1])"
    }
}
Write-Host "RELEASE_PACKAGE=PASS $dist"
