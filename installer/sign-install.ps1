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
        [string]$PreferredPath
    )

    if ($PreferredPath -and (Test-Path -LiteralPath $PreferredPath)) {
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    $command = Get-Command $ToolName -ErrorAction SilentlyContinue
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
        $candidate = Get-ChildItem -Path $root -Recurse -Filter $ToolName -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "$ToolName not found. Install the WDK or pass the tool path explicitly."
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run in an elevated PowerShell to write HKLM and install the SWC device."
}

if (-not (Test-Path -LiteralPath $DriverDir)) {
    New-Item -ItemType Directory -Path $DriverDir | Out-Null
}

$sourceDll = $null
if ($BuildOutputPath) {
    $sourceDll = $BuildOutputPath
} else {
    $buildDir = Join-Path $BuildRoot (Join-Path "build\$Platform" $Configuration)
    $sourceDll = Join-Path $buildDir "AecApo.dll"
}
if (-not (Test-Path -LiteralPath $sourceDll)) {
    throw "Built DLL not found: $sourceDll. Build $Configuration|$Platform first or pass -BuildOutputPath."
}

$buildObjDir = Join-Path $BuildRoot (Join-Path "build\obj\$Platform" $Configuration)
$sourceComponentInf = Join-Path $buildObjDir "aec3apo_component.inf"
$sourceExtensionInf = Join-Path $buildObjDir "aec3apo_extension.inf"
if (-not (Test-Path -LiteralPath $sourceComponentInf)) {
    throw "Built component INF not found: $sourceComponentInf. Build $Configuration|$Platform first."
}
if (-not (Test-Path -LiteralPath $sourceExtensionInf)) {
    throw "Built extension INF not found: $sourceExtensionInf. Build $Configuration|$Platform first."
}

Copy-Item -LiteralPath $sourceDll -Destination (Join-Path $DriverDir "AecApo.dll") -Force
Copy-Item -LiteralPath $sourceComponentInf -Destination $InfPath -Force
Copy-Item -LiteralPath $sourceExtensionInf -Destination $ExtensionInfPath -Force

if (-not (Test-Path -LiteralPath $InfPath)) {
    throw "INF not found after refresh: $InfPath"
}
if (-not (Test-Path -LiteralPath $ExtensionInfPath)) {
    throw "Extension INF not found after refresh: $ExtensionInfPath"
}

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

    # Trust the cert for driver install.
    if ($TrustStore -eq "LocalMachine") {
        # Already ensured admin above.
    }

    $rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", $TrustStore)
    $rootStore.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $rootStore.Add($cert)
    $rootStore.Close()

    $publisherStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", $TrustStore)
    $publisherStore.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $publisherStore.Add($cert)
    $publisherStore.Close()
}

$inf2cat = Resolve-ToolPath -ToolName "Inf2Cat.exe" -PreferredPath $Inf2CatPath
$signtool = Resolve-ToolPath -ToolName "signtool.exe" -PreferredPath $SignToolPath

$catPath = Join-Path $DriverDir "aec3apo.cat"
if (Test-Path -LiteralPath $catPath) {
    Remove-Item -LiteralPath $catPath -Force
}
& $inf2cat /driver:$DriverDir /os:$Inf2CatOs | Write-Host

if (-not (Test-Path -LiteralPath $catPath)) {
    throw "Catalog not generated: $catPath"
}

$sigArgs = @("sign", "/fd", "SHA256", "/f", $PfxPath)
if ($PfxPassword) {
    $sigArgs += @("/p", $PfxPassword)
}
$sigArgs += @("/tr", $TimestampUrl, "/td", "SHA256", $catPath)

& $signtool @sigArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& pnputil /add-driver $InfPath /install
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& pnputil /add-driver $ExtensionInfPath /install
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Restart-Service Audiosrv -Force
Restart-Service AudioEndpointBuilder -Force
