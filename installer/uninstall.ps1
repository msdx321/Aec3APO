param(
    [string]$InfPath = (Join-Path $PSScriptRoot "aec3apo_component.inf"),
    [string]$ExtensionInfPath = (Join-Path $PSScriptRoot "aec3apo_extension.inf"),
    [switch]$Force,
    [string]$DevconPath = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe",
    [string]$CertSubject = "CN=AEC3APO Test",
    [ValidateSet("CurrentUser","LocalMachine")]
    [string]$TrustStore = "LocalMachine"
)

if (-not (Test-Path $DevconPath)) {
    throw "devcon.exe not found: $DevconPath"
}

$infPaths = @($InfPath, $ExtensionInfPath) | Where-Object { $_ }
$lines = & pnputil /enum-drivers

foreach ($inf in $infPaths) {
    if (-not (Test-Path $inf)) {
        Write-Host "INF not found: $inf"
        continue
    }

    $infName = Split-Path $inf -Leaf
    $publishedNames = @()
    $published = $null

    foreach ($line in $lines) {
        if ($line -match 'Published Name\s*:\s*(\S+)') {
            $published = $Matches[1]
            continue
        }
        if ($line -match 'Original Name\s*:\s*(\S+)') {
            if ($published -and $Matches[1].Equals($infName, [System.StringComparison]::OrdinalIgnoreCase)) {
                $publishedNames += $published
            }
        }
    }

    $publishedNames = $publishedNames | Select-Object -Unique
    if ($publishedNames.Count -eq 0) {
        Write-Host "No drivers found for $infName."
        continue
    }

    foreach ($name in $publishedNames) {
        if ($Force) {
            & pnputil /delete-driver $name /uninstall /force
        } else {
            & pnputil /delete-driver $name /uninstall
        }
    }
}

& $DevconPath remove "SWC\VEN_MSDX&AUDIO_EFFECTPACK_AECAPO"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($TrustStore -eq "LocalMachine" -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run in an elevated PowerShell to remove certificates from LocalMachine."
}

$storesToClean = @(
    @{ Name = "My"; Location = "CurrentUser" },
    @{ Name = "Root"; Location = $TrustStore },
    @{ Name = "TrustedPublisher"; Location = $TrustStore }
)

foreach ($entry in $storesToClean) {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($entry.Name, $entry.Location)
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $toRemove = $store.Certificates | Where-Object { $_.Subject -eq $CertSubject }
    foreach ($cert in $toRemove) {
        $store.Remove($cert)
    }
    $store.Close()
}
