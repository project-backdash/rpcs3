# Fetches what build-rpcs3.ps1 needs into this clone: the archives in dependencies.lock.json, each checked against its
# hash, and the submodules of upstream's Windows build. Every path is inside the repository (build\ and packages\,
# which git ignores). Nothing is installed system-wide; the Vulkan SDK is the one dependency installed separately.
param(
    [switch]$SkipSubmodules,
    [switch]$SkipArchives
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'storage-guard.ps1')
Assert-PbdStorage $repository 'dependencies'
$deps = Join-Path $repository 'build\deps'
$downloads = Join-Path $deps 'downloads'
$manifest = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') | ConvertFrom-Json
$actualCommit = (& git -C $repository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine the repository HEAD' }
& git -C $repository merge-base --is-ancestor $manifest.rpcs3_commit HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Dependency lock requires RPCS3 base $($manifest.rpcs3_commit); found unrelated HEAD $actualCommit"
}
$dependencyDefinitions = @('.gitmodules', 'llvm.lock', '3rdparty', '.ci/setup-windows.sh', '.github/workflows/rpcs3.yml')
& git -C $repository diff --quiet $manifest.rpcs3_commit -- @dependencyDefinitions
if ($LASTEXITCODE -ne 0) {
    throw 'RPCS3 dependency definitions differ from the lock base; review and update the dependency lock first'
}
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

function Test-ArtifactHash($path, $artifact) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    if ($artifact.sha256) {
        return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -eq $artifact.sha256
    }
    # NuGet publishes a base64 SHA-512 rather than a hex digest.
    $hex = (Get-FileHash -Algorithm SHA512 -LiteralPath $path).Hash
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    return [Convert]::ToBase64String($bytes) -eq $artifact.sha512Base64
}

function Get-VerifiedArtifact($artifact) {
    $file = Join-Path $downloads ([Uri]$artifact.url).Segments[-1]
    if (-not (Test-ArtifactHash $file $artifact)) {
        Write-Host "Downloading $($artifact.name) $($artifact.version)"
        & curl.exe --fail --location --retry 3 --silent --show-error --output $file $artifact.url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($artifact.name)" }
        if (-not (Test-ArtifactHash $file $artifact)) { throw "Checksum mismatch: $file" }
    }
    Write-Host "Verified $($artifact.name)"
    return $file
}

function Test-RestoredArtifact($marker, $artifact) {
    try {
        $restored = Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json
    } catch {
        return $false
    }
    if ($restored -isnot [pscustomobject] -or
        @($restored.PSObject.Properties).Count -ne @($artifact.PSObject.Properties).Count) {
        return $false
    }
    foreach ($property in $artifact.PSObject.Properties) {
        $recorded = $restored.PSObject.Properties[$property.Name]
        if ($null -eq $recorded -or $recorded.Value -cne $property.Value) { return $false }
    }
    return $true
}

if (-not $SkipArchives) {
    $sevenArtifact = $manifest.artifacts | Where-Object name -eq '7zr'
    $sevenArchive = Get-VerifiedArtifact $sevenArtifact
    $seven = Join-Path $repository $sevenArtifact.destination
    Copy-Item -LiteralPath $sevenArchive -Destination $seven -Force
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($artifact in $manifest.artifacts | Where-Object name -ne '7zr') {
        $archive = Get-VerifiedArtifact $artifact
        $destination = Join-Path $repository $artifact.destination
        # Only a completed extraction of the current locked artifact can be reused.
        $marker = Join-Path $deps ($artifact.name + '.restored')
        if ((Test-Path -LiteralPath $destination) -and (Test-RestoredArtifact $marker $artifact)) {
            Write-Host "Already restored $($artifact.name)"
            continue
        }
        if ($artifact.name -eq 'googletest') {
            $destination = [IO.Path]::GetFullPath($destination)
            $staging = Join-Path $deps ('googletest-' + [Guid]::NewGuid().ToString('N'))
            foreach ($path in @($destination, $staging)) {
                if (-not $path.StartsWith($repository + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Dependency extraction path is outside the repository: $path"
                }
            }
            try {
                [System.IO.Compression.ZipFile]::ExtractToDirectory($archive, $staging)
                if (Test-Path -LiteralPath $destination) {
                    Remove-Item -LiteralPath $destination -Recurse -Force
                }
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
                Move-Item -LiteralPath $staging -Destination $destination
            } finally {
                if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
            }
        } else {
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
            & $seven x -y '-bso0' '-bsp0' $archive "-o$destination"
            if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $($artifact.name)" }
        }
        $artifact | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $marker
        Write-Host "Restored $($artifact.name)"
    }
}

if (-not $SkipSubmodules) {
    # As upstream's Windows CI at the base: LLVM comes prebuilt from the lock, and FAudio and Feral GameMode are not part
    # of the Windows MSVC build.
    $paths = @(Get-Content -LiteralPath (Join-Path $repository '.gitmodules') |
        Where-Object { $_ -match '^\s*path\s*=' -and $_ -notmatch 'FAudio|llvm|feralinteractive' } |
        ForEach-Object { ($_ -split '=', 2)[1].Trim() })
    & git -C $repository submodule update --init --depth=1 --jobs=4 -- @paths
    if ($LASTEXITCODE -ne 0) { throw 'Submodule restoration failed' }
}

Write-Host "QTDIR=$(Join-Path $deps 'Qt\6.11.2\msvc2022_64')"
Write-Host "Dependencies restored. Install the Vulkan SDK 1.4.341.1 to $(Join-Path $deps 'VulkanSDK\1.4.341.1'),"
Write-Host 'or pass its folder to build-rpcs3.ps1 with -VulkanSdk.'
