param(
    [string]$InfPath = (Join-Path $PSScriptRoot "aec3apo_component.inf"),
    [string]$ExtensionInfPath = (Join-Path $PSScriptRoot "aec3apo_extension.inf"),
    [switch]$Force,
    [string]$DevconPath,
    [string]$CertSubject = "CN=AEC3APO Test",
    [ValidateSet("CurrentUser","LocalMachine")]
    [string]$TrustStore = "LocalMachine"
)

function Test-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-OptionalDevcon {
    param([string]$PreferredPath)

    if ($PreferredPath -and (Test-Path -LiteralPath $PreferredPath)) {
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    $command = Get-Command "devcon.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $kitRoots = @()
    if (${env:ProgramFiles(x86)}) {
        $kitRoots += Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
    }
    if ($env:ProgramFiles) {
        $kitRoots += Join-Path $env:ProgramFiles "Windows Kits\10"
    }
    $kitRoots = $kitRoots | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($root in $kitRoots) {
        $candidate = Get-ChildItem -Path $root -Recurse -Filter "devcon.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    return $null
}

function Get-PublishedDriverNames {
    param([string]$OriginalName)

    $drivers = @()
    $current = @{}
    $lines = & pnputil /enum-drivers
    foreach ($line in $lines) {
        if ($line -match '^\s*$') {
            if ($current.PublishedName -or $current.OriginalName) {
                $drivers += New-Object psobject -Property $current
            }
            $current = @{}
            continue
        }

        if ($line -match 'Published Name\s*:\s*(\S+)') {
            $current.PublishedName = $Matches[1]
            continue
        }

        if ($line -match 'Original Name\s*:\s*(\S+)') {
            $current.OriginalName = $Matches[1]
            continue
        }
    }

    if ($current.PublishedName -or $current.OriginalName) {
        $drivers += New-Object psobject -Property $current
    }

    $drivers |
        Where-Object { $_.PublishedName -and $_.OriginalName -and $_.OriginalName.Equals($OriginalName, [System.StringComparison]::OrdinalIgnoreCase) } |
        Select-Object -ExpandProperty PublishedName -Unique
}

if (-not (Test-Administrator)) {
    throw "This script must be run in an elevated PowerShell to uninstall drivers and update certificate stores."
}

$infPaths = @($ExtensionInfPath, $InfPath) | Where-Object { $_ }

foreach ($inf in $infPaths) {
    if (-not (Test-Path -LiteralPath $inf)) {
        Write-Host "INF not found: $inf"
        continue
    }

    $infName = Split-Path $inf -Leaf
    $publishedNames = @(Get-PublishedDriverNames -OriginalName $infName)
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
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to delete driver package $name for $infName."
        }
    }
}

$resolvedDevcon = Resolve-OptionalDevcon -PreferredPath $DevconPath
if ($resolvedDevcon) {
    & $resolvedDevcon remove "SWC\VEN_MSDX&AUDIO_EFFECTPACK_AECAPO"
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "devcon did not remove the SWC device. Driver packages were still processed."
    }
} else {
    Write-Host "devcon.exe not found. Skipping explicit SWC device removal."
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

Restart-Service Audiosrv -Force
Restart-Service AudioEndpointBuilder -Force
