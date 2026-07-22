[CmdletBinding()]
param(
    # The assembled Runtime tree (contains bin/64bit/braidcast.exe, obs-plugins,
    # data). In CI this is the unzipped CPack zip; locally it is
    # build_x64/rundir/<config>.
    [Parameter(Mandatory)]
    [string] $RuntimeDir,

    # Destination .msix path. Parent dir is created if missing.
    [Parameter(Mandatory)]
    [string] $OutFile,

    # Numeric x.y.z; a 4th ".0" revision is appended for the MSIX Identity.
    # Omitted -> derived from `git describe --tags --long`.
    [string] $Version,

    # Store Identity. Dev defaults produce a locally-sideloadable package; the
    # real values come from Partner Center (passed by CI from repo variables).
    [string] $IdentityName = 'Braidcast.Dev',
    [string] $Publisher = 'CN=BraidcastDev',
    [string] $PublisherDisplayName = 'Braidcast (Dev)'
)

$ErrorActionPreference = 'Stop'

$ScriptHome = $PSScriptRoot
$ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
$Template = Join-Path $ProjectRoot 'cmake/windows/msix/AppxManifest.xml.in'
$IconSource = Join-Path $ProjectRoot 'frontend/cmake/windows/braidcast.ico'

function Resolve-MsixVersion {
    param([string] $Requested)
    if ( $Requested ) {
        $numeric = $Requested.TrimStart('v')
    } else {
        # Same token shape as Package-Windows.ps1: describe -> "<ver>-<dist>-g<hash>".
        $describe = & git -C $ProjectRoot describe --tags --long 2>$null
        if ( -not $describe ) { $describe = '0.0.0-0-g0000000' }
        $tokens = $describe -split '-'
        $numeric = ($tokens[0..($tokens.Count - 3)] -join '-').TrimStart('v')
    }
    # MSIX Identity Version is 4-part all-numeric. Keep major.minor.patch, force
    # revision 0; drop any pre-release suffix (e.g. -rc1) makeappx would reject.
    $parts = ($numeric -split '[-+]')[0] -split '\.'
    $nums = @('0', '0', '0')
    for ( $i = 0; $i -lt 3 -and $i -lt $parts.Count; $i++ ) {
        $nums[$i] = ([int]($parts[$i] -replace '\D', '0')).ToString()
    }
    return "$($nums[0]).$($nums[1]).$($nums[2]).0"
}

function Get-MakeAppx {
    $binRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if ( -not ( Test-Path $binRoot ) ) {
        throw "Windows SDK not found at $binRoot. Install the Windows 10/11 SDK (makeappx.exe)."
    }
    $found = Get-ChildItem $binRoot -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\makeappx.exe' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if ( -not $found ) {
        throw "makeappx.exe not found under any $binRoot\10.*\x64. Install the Windows SDK."
    }
    return $found
}

# Generate one PNG logo from the app icon, scaled to fit (letterboxed, centered)
# into $Width x $Height. Placeholder-grade for the skeleton; real Store assets at
# submission.
function New-LogoPng {
    param([System.Drawing.Image] $Source, [int] $Width, [int] $Height, [string] $OutPath)
    $bmp = New-Object System.Drawing.Bitmap($Width, $Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.Clear([System.Drawing.Color]::Transparent)
        $scale = [Math]::Min($Width / $Source.Width, $Height / $Source.Height)
        $w = [int]($Source.Width * $scale)
        $h = [int]($Source.Height * $scale)
        $x = [int](($Width - $w) / 2)
        $y = [int](($Height - $h) / 2)
        $g.DrawImage($Source, $x, $y, $w, $h)
        $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $g.Dispose()
        $bmp.Dispose()
    }
}

function New-MsixAssets {
    param([string] $AssetsDir)
    Add-Type -AssemblyName System.Drawing
    New-Item -ItemType Directory -Force -Path $AssetsDir | Out-Null
    # Load the largest frame the .ico offers.
    $src = [System.Drawing.Image]::FromFile($IconSource)
    try {
        $sizes = @{
            'StoreLogo.png'        = @(50, 50)
            'Square44x44Logo.png'  = @(44, 44)
            'Square71x71Logo.png'  = @(71, 71)
            'Square150x150Logo.png' = @(150, 150)
            'Square310x310Logo.png' = @(310, 310)
            'Wide310x150Logo.png'  = @(310, 150)
        }
        foreach ( $name in $sizes.Keys ) {
            $dim = $sizes[$name]
            New-LogoPng -Source $src -Width $dim[0] -Height $dim[1] -OutPath (Join-Path $AssetsDir $name)
        }
    } finally {
        $src.Dispose()
    }
}

function Package {
    if ( -not ( Test-Path $Template ) ) { throw "Manifest template missing: $Template" }
    if ( -not ( Test-Path $IconSource ) ) { throw "Icon source missing: $IconSource" }
    $exe = Join-Path $RuntimeDir 'bin/64bit/braidcast.exe'
    if ( -not ( Test-Path $exe ) ) {
        throw "RuntimeDir does not look like a Braidcast tree (missing $exe)"
    }

    $msixVersion = Resolve-MsixVersion -Requested $Version
    Write-Host "MSIX Identity: Name=$IdentityName Publisher=$Publisher Version=$msixVersion"

    # Clean staging layout.
    $layout = Join-Path ([System.IO.Path]::GetTempPath()) "braidcast-msix-$([System.IO.Path]::GetRandomFileName())"
    if ( Test-Path $layout ) { Remove-Item $layout -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $layout | Out-Null

    Write-Host "Staging Runtime tree -> $layout"
    Copy-Item -Path (Join-Path $RuntimeDir '*') -Destination $layout -Recurse -Force

    # The portable marker must NOT be in the package, or config resolves to the
    # exe dir instead of the MSIX-virtualized %APPDATA%.
    $marker = Join-Path $layout 'bin/64bit/braidcast_portable.txt'
    if ( Test-Path $marker ) {
        Write-Warning "Removing stray braidcast_portable.txt from MSIX layout"
        Remove-Item $marker -Force
    }

    # Configure the manifest from the template.
    $manifest = (Get-Content $Template -Raw).
        Replace('@BRAIDCAST_MSIX_NAME@', $IdentityName).
        Replace('@BRAIDCAST_MSIX_PUBLISHER@', $Publisher).
        Replace('@BRAIDCAST_MSIX_PUBLISHER_DISPLAY@', $PublisherDisplayName).
        Replace('@BRAIDCAST_MSIX_VERSION@', $msixVersion)
    $manifest | Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Encoding UTF8

    # Generate the visual assets.
    New-MsixAssets -AssetsDir (Join-Path $layout 'Assets')

    # Pack.
    $makeappx = Get-MakeAppx
    Write-Host "makeappx: $makeappx"
    $outDir = Split-Path -Parent $OutFile
    if ( $outDir -and -not ( Test-Path $outDir ) ) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }
    & $makeappx pack /o /d $layout /p $OutFile
    if ( $LASTEXITCODE -ne 0 ) { throw "makeappx pack failed ($LASTEXITCODE)" }
    if ( -not ( Test-Path $OutFile ) ) { throw "makeappx reported success but $OutFile is missing" }

    # Validate: unpack round-trip; assert the manifest + exe + StoreLogo survived.
    $verify = Join-Path ([System.IO.Path]::GetTempPath()) "braidcast-msix-verify-$([System.IO.Path]::GetRandomFileName())"
    & $makeappx unpack /o /d $verify /p $OutFile
    if ( $LASTEXITCODE -ne 0 ) { throw "makeappx unpack (validation) failed ($LASTEXITCODE)" }
    foreach ( $rel in @('AppxManifest.xml', 'bin/64bit/braidcast.exe', 'Assets/StoreLogo.png') ) {
        if ( -not ( Test-Path (Join-Path $verify $rel) ) ) {
            throw "Validation failed: $rel missing from packed MSIX"
        }
    }
    Remove-Item $verify -Recurse -Force
    Remove-Item $layout -Recurse -Force

    Write-Host "MSIX written and validated: $OutFile"
}

Package
