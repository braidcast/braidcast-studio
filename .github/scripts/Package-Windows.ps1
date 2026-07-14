[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "obs-studio requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The obs-studio packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    Install-BuildDependencies -WingetFile "${ScriptHome}/.Wingetfile"

    $GitDescription = Invoke-External git describe --tags --long
    $Tokens = ($GitDescription -split '-')
    $CommitVersion = $Tokens[0..$($Tokens.Count - 3)] -join '-'
    $CommitHash = $($Tokens[-1]).SubString(1)
    $CommitDistance = $Tokens[-2]

    if ( $CommitDistance -gt 0 ) {
        $OutputName = "braidcast-${CommitVersion}-${CommitHash}"
    } else {
        $OutputName = "braidcast-${CommitVersion}"
    }

    $CpackArgs = @(
        '-C', "${Configuration}"
    )

    if ( $DebugPreference -eq 'Continue' ) {
        $CpackArgs += ('--verbose')
    }

    # The NSIS CPack generator (portable ZIP + system installer) needs makensis.
    # The GitHub windows-2022 runner ships NSIS; install defensively only if a
    # runner image ever drops it so the installer artifact is never silently
    # skipped. CPack locates makensis via the registry/standard install path, so
    # PATH does not need updating here.
    if (
        -not ( Get-Command makensis -ErrorAction SilentlyContinue ) -and
        -not ( Test-Path 'C:\Program Files (x86)\NSIS\makensis.exe' )
    ) {
        Log-Information 'NSIS (makensis) not found; installing via Chocolatey...'
        choco install nsis --yes --no-progress
    }

    Log-Group "Packaging obs-studio..."

    Push-Location -Stack PackageTemp "build_${Target}"

    cpack @CpackArgs

    # CPack emits both generators named from CPACK_PACKAGE_FILE_NAME:
    # braidcast-<canonical>-windows-<arch>.{zip,exe}. Rename each to the
    # git-describe OutputName so the release/upload globs stay version-agnostic.
    $Zip = Get-ChildItem -Filter "braidcast-*-windows-${Target}.zip" -File | Select-Object -First 1
    if ( -not $Zip ) {
        throw "CPack did not produce a braidcast-*-windows-${Target}.zip"
    }
    Move-Item -Path $Zip -Destination "${OutputName}-windows-${Target}.zip" -Force

    $Installer = Get-ChildItem -Filter "braidcast-*-windows-${Target}.exe" -File | Select-Object -First 1
    if ( -not $Installer ) {
        throw "CPack did not produce a braidcast-*-windows-${Target}.exe (NSIS installer)"
    }
    Move-Item -Path $Installer -Destination "${OutputName}-windows-${Target}.exe" -Force

    Pop-Location -Stack PackageTemp
}

Package
