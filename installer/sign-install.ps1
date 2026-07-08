param(
    [string]$InfPath = (Join-Path $PSScriptRoot "aec3apo_component.inf"),
    [string]$ExtensionInfPath = (Join-Path $PSScriptRoot "aec3apo_extension.inf"),
    [string]$DriverDir = $PSScriptRoot,
    [string]$BuildRoot = (Join-Path $PSScriptRoot ".."),
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$BuildOutputPath,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$CertSubject = "CN=AEC3APO Test",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string]$Inf2CatPath = "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.28000.0\\x86\\Inf2Cat.exe",
    [string]$SignToolPath = "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.28000.0\\x64\\signtool.exe",
    [string]$Inf2CatOs = "10_X64",
    [ValidateSet("CurrentUser","LocalMachine")]
    [string]$TrustStore = "LocalMachine",
    [string]$EndpointId
)

function Resolve-ToolPath {
    param(
        [Parameter(Mandatory=$true)]
        [string]$ToolName,
        [string]$PreferredPath,
        [string[]]$Architectures = @("x64", "x86", "arm64", "arm")
    )

    if ($PreferredPath -and (Test-Path -LiteralPath $PreferredPath)) {
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    $command = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $kitTool = Find-WindowsKitTool -ToolName $ToolName -Architectures $Architectures
    if ($kitTool) {
        return $kitTool
    }

    throw "$ToolName not found. Install the WDK or pass the tool path explicitly."
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
                Sort-Object { [version]$_.Name } -Descending

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

function Add-CertificateToStore {
    param(
        [Parameter(Mandatory=$true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
        [Parameter(Mandatory=$true)]
        [string]$StoreName,
        [Parameter(Mandatory=$true)]
        [string]$StoreLocation
    )

    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($StoreName, $StoreLocation)
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $store.Add($Certificate)
    } finally {
        $store.Close()
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory=$true)]
        [string]$FilePath,
        [Parameter(Mandatory=$true)]
        [string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Assert-PathExists {
    param(
        [Parameter(Mandatory=$true)]
        [string]$Path,
        [Parameter(Mandatory=$true)]
        [string]$Message
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw $Message
    }
}

function Test-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
    throw "This script must be run in an elevated PowerShell to write HKLM and install the SWC device."
}

if (-not (Test-Path -LiteralPath $DriverDir)) {
    New-Item -ItemType Directory -LiteralPath $DriverDir | Out-Null
}

$sourceDll = $null
if ($BuildOutputPath) {
    $sourceDll = $BuildOutputPath
} else {
    $buildDir = Join-Path $BuildRoot (Join-Path "build\$Platform" $Configuration)
    $sourceDll = Join-Path $buildDir "AecApo.dll"
}
Assert-PathExists -Path $sourceDll -Message "Built DLL not found: $sourceDll. Build $Configuration|$Platform first or pass -BuildOutputPath."

$buildObjDir = Join-Path $BuildRoot (Join-Path "build\obj\$Platform" $Configuration)
$sourceComponentInf = Join-Path $buildObjDir "aec3apo_component.inf"
$sourceExtensionInf = Join-Path $buildObjDir "aec3apo_extension.inf"
Assert-PathExists -Path $sourceComponentInf -Message "Built component INF not found: $sourceComponentInf. Build $Configuration|$Platform first."
Assert-PathExists -Path $sourceExtensionInf -Message "Built extension INF not found: $sourceExtensionInf. Build $Configuration|$Platform first."

Copy-Item -LiteralPath $sourceDll -Destination (Join-Path $DriverDir "AecApo.dll") -Force
Copy-Item -LiteralPath $sourceComponentInf -Destination $InfPath -Force
Copy-Item -LiteralPath $sourceExtensionInf -Destination $ExtensionInfPath -Force

Assert-PathExists -Path $InfPath -Message "INF not found after refresh: $InfPath"
Assert-PathExists -Path $ExtensionInfPath -Message "Extension INF not found after refresh: $ExtensionInfPath"

if (-not $PfxPath) {
    $PfxPath = Join-Path $DriverDir "aec3apo_test.pfx"
    if (-not $PfxPassword) {
        $PfxPassword = "aec-apo"
    }

    $certStorePath = "Cert:\CurrentUser\My"
    $cert = Get-ChildItem -Path $certStorePath |
        Where-Object { $_.Subject -eq $CertSubject } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
    $minNotAfter = (Get-Date).AddYears(9)
    if (-not $cert -or $cert.NotAfter -lt $minNotAfter) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation $certStorePath -NotAfter (Get-Date).AddYears(10)
    }

    $securePassword = ConvertTo-SecureString -String $PfxPassword -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $PfxPath -Password $securePassword | Out-Null

    Add-CertificateToStore -Certificate $cert -StoreName "Root" -StoreLocation $TrustStore
    Add-CertificateToStore -Certificate $cert -StoreName "TrustedPublisher" -StoreLocation $TrustStore
}

$inf2cat = Resolve-ToolPath -ToolName "Inf2Cat.exe" -PreferredPath $Inf2CatPath -Architectures @("x86", "x64")
$signtool = Resolve-ToolPath -ToolName "signtool.exe" -PreferredPath $SignToolPath -Architectures @("x64", "x86")

$catPath = Join-Path $DriverDir "aec3apo.cat"
if (Test-Path -LiteralPath $catPath) {
    Remove-Item -LiteralPath $catPath -Force
}
Invoke-NativeCommand -FilePath $inf2cat -ArgumentList @("/driver:$DriverDir", "/os:$Inf2CatOs")

Assert-PathExists -Path $catPath -Message "Catalog not generated: $catPath"

$sigArgs = @("sign", "/fd", "SHA256", "/f", $PfxPath)
if ($PfxPassword) {
    $sigArgs += @("/p", $PfxPassword)
}
$sigArgs += @("/tr", $TimestampUrl, "/td", "SHA256", $catPath)

Invoke-NativeCommand -FilePath $signtool -ArgumentList $sigArgs

Invoke-NativeCommand -FilePath "pnputil" -ArgumentList @("/add-driver", $InfPath, "/install")

Invoke-NativeCommand -FilePath "pnputil" -ArgumentList @("/add-driver", $ExtensionInfPath, "/install")

Restart-Service Audiosrv -Force
Restart-Service AudioEndpointBuilder -Force
