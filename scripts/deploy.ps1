#!/usr/bin/env pwsh
#Requires -Version 5.1
# Dev deploy: copy the freshly built AbzuHeadTracking.asi + HeadTracking.ini
# into a detected ABZU install. Game-path detection (env var -> Steam ->
# games.json -> positional arg) matches install.cmd via GamePathDetection.psm1.
# No prompts; exits non-zero if detection or the build artifact is missing.

param(
    [Parameter(Position=0)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [Parameter(Position=1)]
    [string]$GivenPath,
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$RemainingArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = 'SilentlyContinue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectRoot "cameraunlock-core\powershell\DevDeploy.psm1") -Force
Import-Module (Join-Path $projectRoot "cameraunlock-core\powershell\ModDeployment.psm1") -Force

$buildOutput  = Join-Path $projectRoot "build\src\AbzuHeadTracking\$Configuration"
$configFile   = Join-Path $projectRoot 'HeadTracking.ini'
$vendorLoader = Join-Path $projectRoot 'vendor\ultimate-asi-loader\dinput8.dll'

$result = Invoke-DevDeployASILoader `
    -GameId 'abzu' `
    -GameDisplayName 'ABZU' `
    -BuildOutputPath $buildOutput `
    -ModDllName 'AbzuHeadTracking.asi' `
    -ConfigFile $configFile `
    -VendorLoaderDll $vendorLoader `
    -AsiLoaderName 'xinput1_3.dll' `
    -ExtraDlls @() `
    -GivenPath $GivenPath

Write-DeploymentSuccess `
    -ModName "ABZU Head Tracking" `
    -DeployPath $result.DeployedDllPath `
    -RecenterKey "Home" `
    -ToggleKey "End"
