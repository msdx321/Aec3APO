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
    [ValidateSet("CurrentUser","LocalMachine")]
    [string]$TrustStore = "LocalMachine",
    [string]$EndpointId
)

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run in an elevated PowerShell to write HKLM and install the SWC device."
}

if (-not (Test-Path $InfPath)) {
    throw "INF not found: $InfPath"
}
if (-not (Test-Path $ExtensionInfPath)) {
    throw "Extension INF not found: $ExtensionInfPath"
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

$inf2cat = $Inf2CatPath
$signtool = $SignToolPath

if (-not (Test-Path $inf2cat)) {
    throw "Inf2Cat.exe not found: $inf2cat"
}

if (-not (Test-Path $signtool)) {
    throw "signtool.exe not found: $signtool"
}


$sourceDll = $null
if ($BuildOutputPath) {
    $sourceDll = $BuildOutputPath
} else {
    $buildDir = Join-Path $BuildRoot (Join-Path "build\\$Platform" $Configuration)
    $sourceDll = Join-Path $buildDir "AecApo.dll"
    if (-not (Test-Path $sourceDll)) {
        $candidates = Get-ChildItem -Path $BuildRoot -Filter "AecApo.dll" -Recurse -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending
        if ($candidates.Count -gt 0) {
            $sourceDll = $candidates[0].FullName
        }
    }
}
if (-not (Test-Path $sourceDll)) {
    throw "Built DLL not found. Provide -BuildOutputPath or check your build output."
}

$targetDll = Join-Path $DriverDir "AecApo.dll"
Copy-Item -LiteralPath $sourceDll -Destination $targetDll -Force

$catPath = Join-Path $DriverDir "aec3apo.cat"
if (Test-Path $catPath) {
    Remove-Item -LiteralPath $catPath -Force
}
& $inf2cat /driver:$DriverDir /os:10_GE_X64 | Write-Host

if (-not (Test-Path $catPath)) {
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
