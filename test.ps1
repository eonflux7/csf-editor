[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',

    [switch]$CoreOnly,

    [switch]$NoBuild,

    # Also build csf-editor and run the UI tests (label ui).
    [switch]$Ui
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE."
    }
}

$configurePreset = if ($CoreOnly) { 'core' } else { 'default' }
$presetPrefix = if ($CoreOnly) { 'core-' } else { '' }
$testPreset = $presetPrefix + $Config.ToLowerInvariant()
$buildDirectory = if ($CoreOnly) { 'build-core' } else { 'build' }
$cachePath = Join-Path $PSScriptRoot "$buildDirectory/CMakeCache.txt"

if ($CoreOnly -and $Ui) {
    throw '-Ui needs the GUI build; drop -CoreOnly.'
}

Push-Location $PSScriptRoot
try {
    if (-not (Test-Path -LiteralPath $cachePath)) {
        Invoke-Native 'cmake' @('--preset', $configurePreset)
    }

    if (-not $NoBuild) {
        $targets = @('rws_core_tests', 'csf_authoring_tests', 'fake_pakman')
        if ($Ui) { $targets += @('csf-editor', 'rwsman_ui_fixture') }
        Invoke-Native 'cmake' (@('--build', '--preset', $testPreset, '--target') + $targets + @('--parallel'))
    }

    if ($Ui) {
        Invoke-Native 'ctest' @('--preset', $testPreset)
    }
    else {
        Invoke-Native 'ctest' @('--preset', $testPreset, '--label-exclude', 'ui')
    }
}
finally {
    Pop-Location
}
