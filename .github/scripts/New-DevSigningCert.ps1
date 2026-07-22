[CmdletBinding()]
param(
    # Must match the -Publisher used to pack the .msix (default matches
    # Package-Msix.ps1's dev default).
    [string] $Publisher = 'CN=BraidcastDev',

    # The .msix to sign and (optionally) install.
    [Parameter(Mandatory)]
    [string] $MsixPath,

    # Also Add-AppxPackage after signing.
    [switch] $Install
)

$ErrorActionPreference = 'Stop'

if ( -not ( Test-Path $MsixPath ) ) { throw "MSIX not found: $MsixPath" }

# Reuse an existing dev cert with this subject if present; else make one.
$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $Publisher -and $_.HasPrivateKey } |
    Select-Object -First 1
if ( -not $cert ) {
    Write-Host "Creating self-signed dev cert $Publisher"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher `
        -KeyUsage DigitalSignature -FriendlyName 'Braidcast Dev Signing' `
        -CertStoreLocation Cert:\CurrentUser\My `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}

# Trust it for package validation (TrustedPeople). Requires elevation.
$exported = Join-Path $Env:TEMP 'braidcast-dev-cert.cer'
Export-Certificate -Cert $cert -FilePath $exported | Out-Null
Import-Certificate -FilePath $exported -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null

# Locate signtool (same SDK bin as makeappx).
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
    Where-Object { $_.Name -match '^10\.' } |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if ( -not $signtool ) { throw 'signtool.exe not found in the Windows SDK.' }

& $signtool sign /fd SHA256 /a /sha1 $cert.Thumbprint $MsixPath
if ( $LASTEXITCODE -ne 0 ) { throw "signtool sign failed ($LASTEXITCODE)" }
Write-Host "Signed $MsixPath with $($cert.Thumbprint)"

if ( $Install ) {
    Write-Host "Installing $MsixPath"
    Add-AppxPackage -Path $MsixPath
    Write-Host 'Installed. Launch Braidcast from the Start menu to verify.'
}
