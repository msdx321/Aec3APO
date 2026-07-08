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

function Get-WindowsKitRoots {
    $kitRoots = @()
    if (${env:ProgramFiles(x86)}) {
        $kitRoots += Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
    }
    if ($env:ProgramFiles) {
        $kitRoots += Join-Path $env:ProgramFiles "Windows Kits\10"
    }
    $kitRoots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
}

function Find-WindowsKitTool {
    param(
        [Parameter(Mandatory=$true)]
        [string]$ToolName,
        [string[]]$Architectures = @("x64", "x86", "arm64", "arm")
    )

    foreach ($root in Get-WindowsKitRoots) {
        $binRoot = Join-Path $root "bin"
        if (Test-Path -LiteralPath $binRoot) {
            $versionDirs = Get-ChildItem -LiteralPath $binRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
                Sort-Object Name -Descending

            foreach ($versionDir in $versionDirs) {
                foreach ($arch in $Architectures) {
                    $candidate = Join-Path (Join-Path $versionDir.FullName $arch) $ToolName
                    if (Test-Path -LiteralPath $candidate) {
                        return (Resolve-Path -LiteralPath $candidate).Path
                    }
                }
            }
        }

        $candidate = Get-ChildItem -LiteralPath $root -Recurse -File -Filter $ToolName -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    return $null
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

    $kitTool = Find-WindowsKitTool -ToolName "devcon.exe" -Architectures @("x64", "x86", "arm64", "arm")
    if ($kitTool) {
        return $kitTool
    }

    return $null
}

function Remove-CertificatesFromStore {
    param(
        [Parameter(Mandatory=$true)]
        [string]$StoreName,
        [Parameter(Mandatory=$true)]
        [string]$StoreLocation,
        [Parameter(Mandatory=$true)]
        [string]$Subject
    )

    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($StoreName, $StoreLocation)
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $toRemove = $store.Certificates | Where-Object { $_.Subject -eq $Subject }
        foreach ($cert in $toRemove) {
            $store.Remove($cert)
        }
    } finally {
        $store.Close()
    }
}

function Get-PublishedDriverNames {
    param([string]$OriginalName)

    $drivers = [System.Collections.Generic.List[object]]::new()
    $current = @{}
    $lines = & pnputil /enum-drivers
    foreach ($line in $lines) {
        if ($line -match '^\s*$') {
            if ($current.PublishedName -or $current.OriginalName) {
                [void]$drivers.Add([pscustomobject]$current)
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
        [void]$drivers.Add([pscustomobject]$current)
    }

    $drivers |
        Where-Object { $_.PublishedName -and $_.OriginalName -and $_.OriginalName.Equals($OriginalName, [System.StringComparison]::OrdinalIgnoreCase) } |
        Select-Object -ExpandProperty PublishedName -Unique
}

function Invoke-DeleteDriverPackage {
    param(
        [Parameter(Mandatory=$true)]
        [string]$PublishedName,
        [Parameter(Mandatory=$true)]
        [string]$InfName,
        [switch]$Force
    )

    $arguments = @("/delete-driver", $PublishedName, "/uninstall")
    if ($Force) {
        $arguments += "/force"
    }

    & pnputil @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to delete driver package $PublishedName for $InfName."
    }
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
        Invoke-DeleteDriverPackage -PublishedName $name -InfName $infName -Force:$Force
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
    Remove-CertificatesFromStore -StoreName $entry.Name -StoreLocation $entry.Location -Subject $CertSubject
}

Restart-Service Audiosrv -Force
Restart-Service AudioEndpointBuilder -Force
