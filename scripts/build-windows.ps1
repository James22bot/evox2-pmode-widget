#Requires -Version 5.1

param(
    [Parameter(Mandatory = $true)][string]$Zig
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $root

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

$zigVersion = @(& $Zig version)
if ($LASTEXITCODE -ne 0 -or $zigVersion.Count -ne 1 -or $zigVersion[0].Trim() -ne '0.16.0') {
    throw 'Zig 0.16.0 is required.'
}

$module = Join-Path $root 'third_party\PawnIO.Modules-0.1.6\LpcACPIEC.bin'
$moduleHash = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash.ToLowerInvariant()
if ($moduleHash -ne 'c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906') {
    throw 'PawnIO module hash mismatch.'
}

$build = Join-Path $root 'build\windows'
[IO.Directory]::CreateDirectory($build) | Out-Null
$common = @(
    'c++', '-target', 'x86_64-windows-gnu', '-std=c++20', '-O2',
    '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wno-nullability-completeness',
    '-fstack-protector-strong', '-DUNICODE', '-D_UNICODE', '-Iinclude'
)

Invoke-Checked $Zig ($common + @(
    'src/core.cpp', 'tests/test_core.cpp',
    '-o', (Join-Path $build 'test_core.exe')
))
Invoke-Checked (Join-Path $build 'test_core.exe') @()

Invoke-Checked $Zig ($common + @(
    'src/core.cpp', 'src/overlay_model.cpp', 'tests/test_overlay_model.cpp',
    '-o', (Join-Path $build 'test_overlay_model.exe')
))
Invoke-Checked (Join-Path $build 'test_overlay_model.exe') @()

$resource = Join-Path $build 'app_resources.o'
Invoke-Checked $Zig @(
    'rc', '/nologo', '/c', '65001', '/:auto-includes', 'gnu',
    '/:output-format', 'coff', '/:target', 'x86_64', '/i', 'resources',
    '/fo', $resource, 'resources/app.rc'
)

$app = Join-Path $build 'evox2-pmode-overlay.exe'
Invoke-Checked $Zig ($common + @(
    '-s',
    'src/core.cpp', 'src/overlay_model.cpp', 'src/windows_ec_backend.cpp', 'src/overlay_main.cpp',
    $resource,
    '-lbcrypt', '-ladvapi32', '-lshell32', '-lgdi32', '-luser32',
    '-Wl,--subsystem,windows', '-o', $app
))

if (-not (Test-Path -LiteralPath $app -PathType Leaf)) {
    throw 'Windows application build output is missing.'
}
Write-Host "WINDOWS_BUILD=PASS $app"
