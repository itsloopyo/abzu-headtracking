#!/usr/bin/env pwsh
#Requires -Version 5.1
# Custom packaging for Abzu Head Tracking (C++ project, no .csproj).
# Produces:
#   - AbzuHeadTracking-v{version}-installer.zip (GitHub Release)
#   - AbzuHeadTracking-v{version}-nexus.zip (Nexus extract-to-game-folder)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = 'SilentlyContinue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectDir "cameraunlock-core\powershell\ReleaseWorkflow.psm1") -Force

# Version comes from the project() line in CMakeLists.txt. Single source of truth.
$cmakeLists = Get-Content (Join-Path $projectDir "CMakeLists.txt") -Raw
if ($cmakeLists -notmatch 'project\(\s*AbzuHeadTracking\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not parse VERSION from CMakeLists.txt project() declaration."
}
$version = $Matches[1]

Write-Host "=== Abzu Head Tracking - Package Release ===" -ForegroundColor Magenta
Write-Host "Version: $version" -ForegroundColor Cyan

$releaseDir = Join-Path $projectDir "release"
if (-not (Test-Path $releaseDir)) {
    New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
}

# --- Required sources --------------------------------------------------------

$asiPath = Join-Path $projectDir "build/src/AbzuHeadTracking/Release/AbzuHeadTracking.asi"
if (-not (Test-Path $asiPath)) { throw "AbzuHeadTracking.asi not found at: $asiPath" }

$iniPath = Join-Path $projectDir "HeadTracking.ini"
if (-not (Test-Path $iniPath)) { throw "HeadTracking.ini not found at: $iniPath" }

$vendorAsiDir = Join-Path $projectDir "vendor/ultimate-asi-loader"
$vendorAsiDll = Join-Path $vendorAsiDir "dinput8.dll"
if (-not (Test-Path $vendorAsiDll)) { throw "Bundled ASI loader missing: $vendorAsiDll" }

$scriptsDir = Join-Path $projectDir "scripts"
foreach ($script in @("install.cmd", "uninstall.cmd")) {
    if (-not (Test-Path (Join-Path $scriptsDir $script))) {
        throw "Required script not found: $script"
    }
}

# --- GitHub Release ZIP ------------------------------------------------------

Write-Host ""
Write-Host "--- GitHub Release ZIP ---" -ForegroundColor Yellow

$ghStagingDir = Join-Path $releaseDir "staging-github"
if (Test-Path $ghStagingDir) { Remove-Item -Recurse -Force $ghStagingDir }
New-Item -ItemType Directory -Path $ghStagingDir -Force | Out-Null

foreach ($script in @("install.cmd", "uninstall.cmd")) {
    Copy-Item (Join-Path $scriptsDir $script) -Destination $ghStagingDir -Force
    Write-Host "  $script" -ForegroundColor Green
}

$pluginsDir = Join-Path $ghStagingDir "plugins"
New-Item -ItemType Directory -Path $pluginsDir -Force | Out-Null
Copy-Item $asiPath -Destination $pluginsDir -Force
Write-Host "  plugins/AbzuHeadTracking.asi" -ForegroundColor Green
Copy-Item $iniPath -Destination $pluginsDir -Force
Write-Host "  plugins/HeadTracking.ini" -ForegroundColor Green

$ghVendorDir = Join-Path $ghStagingDir "vendor/ultimate-asi-loader"
New-Item -ItemType Directory -Path $ghVendorDir -Force | Out-Null
foreach ($vendorFile in @("dinput8.dll", "LICENSE", "README.md")) {
    $src = Join-Path $vendorAsiDir $vendorFile
    if (Test-Path $src) {
        Copy-Item $src -Destination $ghVendorDir -Force
        Write-Host "  vendor/ultimate-asi-loader/$vendorFile" -ForegroundColor Green
    }
}

foreach ($doc in @("README.md", "LICENSE", "CHANGELOG.md", "THIRD-PARTY-NOTICES.md")) {
    $docPath = Join-Path $projectDir $doc
    if (Test-Path $docPath) {
        Copy-Item $docPath -Destination $ghStagingDir -Force
        Write-Host "  $doc" -ForegroundColor Green
    }
}

# launcher-manifest.json sits at the ZIP root - the contract lopari reads.
# Stamp the real release version into mod_info.version (single source of
# truth is the CMakeLists project() declaration parsed above).
$manifestSrc = Join-Path $projectDir "launcher-manifest.json"
if (-not (Test-Path $manifestSrc)) { throw "launcher-manifest.json not found at: $manifestSrc" }
$manifest = Get-Content $manifestSrc -Raw | ConvertFrom-Json
$manifest.mod_info.version = $version
$manifestJson = $manifest | ConvertTo-Json -Depth 12
# BOM-less UTF-8: PS 5.1 Set-Content -Encoding UTF8 prepends a BOM that trips
# strict JSON parsers (System.Text.Json, Python json). Write raw bytes instead.
[System.IO.File]::WriteAllText(
    (Join-Path $ghStagingDir "launcher-manifest.json"),
    $manifestJson,
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "  launcher-manifest.json (version $version)" -ForegroundColor Green

Copy-SharedBundle -StagingDir $ghStagingDir -NoRefresh

$ghZipName = "AbzuHeadTracking-v$version-installer.zip"
$ghZipPath = Join-Path $releaseDir $ghZipName
if (Test-Path $ghZipPath) { Remove-Item $ghZipPath -Force }

Push-Location $ghStagingDir
try { Compress-Archive -Path ".\*" -DestinationPath $ghZipPath -Force }
finally { Pop-Location }
Remove-Item -Recurse -Force $ghStagingDir

$ghZipSize = (Get-Item $ghZipPath).Length / 1KB
Write-Host ("  $ghZipPath ({0:N1} KB)" -f $ghZipSize) -ForegroundColor Green

# --- Nexus ZIP ---------------------------------------------------------------

Write-Host ""
Write-Host "--- Nexus Mods ZIP ---" -ForegroundColor Yellow

$nexusStagingDir = Join-Path $releaseDir "staging-nexus"
if (Test-Path $nexusStagingDir) { Remove-Item -Recurse -Force $nexusStagingDir }

# Mirror game directory structure: AbzuGame\Binaries\Win64\
$nexusGameDir = Join-Path $nexusStagingDir "AbzuGame\Binaries\Win64"
New-Item -ItemType Directory -Path $nexusGameDir -Force | Out-Null

Copy-Item $asiPath -Destination $nexusGameDir -Force
Write-Host "  AbzuGame/Binaries/Win64/AbzuHeadTracking.asi" -ForegroundColor Green
Copy-Item $iniPath -Destination $nexusGameDir -Force
Write-Host "  AbzuGame/Binaries/Win64/HeadTracking.ini" -ForegroundColor Green

# Nexus is the deploy subtree only - no vendored loader. Nexus users manage
# their own ASI loader.

$nexusZipName = "AbzuHeadTracking-v$version-nexus.zip"
$nexusZipPath = Join-Path $releaseDir $nexusZipName
if (Test-Path $nexusZipPath) { Remove-Item $nexusZipPath -Force }

Push-Location $nexusStagingDir
try { Compress-Archive -Path ".\*" -DestinationPath $nexusZipPath -Force }
finally { Pop-Location }
Remove-Item -Recurse -Force $nexusStagingDir

$nexusZipSize = (Get-Item $nexusZipPath).Length / 1KB
Write-Host ("  $nexusZipPath ({0:N1} KB)" -f $nexusZipSize) -ForegroundColor Green

Write-Host ""
Write-Host "=== Package Complete ===" -ForegroundColor Magenta
Write-Host ("GitHub Release: $ghZipPath ({0:N1} KB)" -f $ghZipSize) -ForegroundColor Green
Write-Host ("Nexus Mods:     $nexusZipPath ({0:N1} KB)" -f $nexusZipSize) -ForegroundColor Green

Write-Output $ghZipPath
Write-Output $nexusZipPath
