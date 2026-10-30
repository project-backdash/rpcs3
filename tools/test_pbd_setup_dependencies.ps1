$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repository = Split-Path -Parent $PSScriptRoot
$testRoot = [IO.Path]::GetFullPath((Join-Path $repository ('build\dependency-test-' + [Guid]::NewGuid().ToString('N'))))
if (-not $testRoot.StartsWith($repository + '\build\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Test directory is outside the build folder: $testRoot"
}

try {
    $scripts = Join-Path $testRoot 'tools\pbd'
    $downloads = Join-Path $testRoot 'build\deps\downloads'
    $payload = Join-Path $testRoot 'payload'
    foreach ($directory in @($scripts, $downloads, $payload)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }
    foreach ($file in @('setup-dependencies.ps1', 'storage-guard.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "pbd\$file") -Destination $scripts
    }
    # The fixture extracts only a few bytes and uses the real storage check.
    '{"reserve_bytes":0,"job_estimates_bytes":{"dependencies":0}}' |
        Set-Content -LiteralPath (Join-Path $scripts 'storage-policy.json') -Encoding UTF8
    & git init --quiet $testRoot
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the dependency fixture' }
    & git -C $testRoot -c user.name=Build -c user.email=build@localhost commit --allow-empty --quiet -m 'Dependency fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot commit the dependency fixture' }
    $baseCommit = (& git -C $testRoot rev-parse HEAD).Trim()

    Set-Content -LiteralPath (Join-Path $payload 'existing.txt') -Value 'complete' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $payload 'missing.txt') -Value 'restored' -Encoding UTF8
    $archive = Join-Path $downloads 'googletest.zip'
    [System.IO.Compression.ZipFile]::CreateFromDirectory($payload, $archive)
    # This artifact is verified and copied; ZIP extraction does not invoke it.
    $sevenArchive = Join-Path $downloads '7zr.exe'
    Set-Content -LiteralPath $sevenArchive -Value 'unused for ZIP' -Encoding UTF8
    $artifact = [ordered]@{
        name = 'googletest'
        version = '1'
        url = 'https://invalid.local/googletest.zip'
        sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        destination = 'packages/googletest'
    }
    $manifest = [ordered]@{
        rpcs3_commit = $baseCommit
        artifacts = @(
            [ordered]@{
                name = '7zr'
                version = '1'
                url = 'https://invalid.local/7zr.exe'
                sha256 = (Get-FileHash -LiteralPath $sevenArchive -Algorithm SHA256).Hash
                destination = 'build/deps/7zr.exe'
            },
            $artifact
        )
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $scripts 'dependencies.lock.json') -Encoding UTF8
    $destination = Join-Path $testRoot $artifact.destination
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Set-Content -LiteralPath (Join-Path $destination 'existing.txt') -Value 'partial' -Encoding UTF8
    $unrelated = Join-Path $testRoot 'packages\unrelated.txt'
    Set-Content -LiteralPath $unrelated -Value 'keep' -Encoding UTF8

    & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules
    foreach ($file in @('existing.txt', 'missing.txt')) {
        $expected = [IO.File]::ReadAllBytes((Join-Path $payload $file))
        $actual = [IO.File]::ReadAllBytes((Join-Path $destination $file))
        if ([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($expected)) {
            throw "Retry did not restore $file"
        }
    }
    $marker = Join-Path $testRoot 'build\deps\googletest.restored'
    if (-not (Test-Path -LiteralPath $marker)) {
        throw 'Successful extraction did not publish its marker'
    }
    if ((Get-Content -Raw -LiteralPath $unrelated).Trim() -cne 'keep') {
        throw 'Restoration changed an unrelated package'
    }
    Set-Content -LiteralPath (Join-Path $destination 'existing.txt') -Value 'cached' -Encoding UTF8
    & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules
    if ((Get-Content -Raw -LiteralPath (Join-Path $destination 'existing.txt')).Trim() -cne 'cached') {
        throw 'A current marker did not reuse the extracted dependency'
    }

    Set-Content -LiteralPath (Join-Path $payload 'existing.txt') -Value 'updated' -Encoding UTF8
    Remove-Item -LiteralPath $archive
    [System.IO.Compression.ZipFile]::CreateFromDirectory($payload, $archive)
    $artifact.version = '2'
    $artifact.sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $scripts 'dependencies.lock.json') -Encoding UTF8
    & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules
    if ((Get-Content -Raw -LiteralPath (Join-Path $destination 'existing.txt')).Trim() -cne 'updated') {
        throw 'A changed artifact at the same destination was not restored'
    }
    $recorded = Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json
    if ($recorded.sha256 -cne $artifact.sha256 -or $recorded.version -cne '2') {
        throw 'Restoration did not replace the obsolete marker'
    }

    foreach ($invalid in @('{', 'null', '[]', '{"name":"googletest"}')) {
        Set-Content -LiteralPath $marker -Value $invalid -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $destination 'existing.txt') -Value 'partial' -Encoding UTF8
        & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules
        if ((Get-Content -Raw -LiteralPath (Join-Path $destination 'existing.txt')).Trim() -cne 'updated') {
            throw "An invalid marker prevented restoration: $invalid"
        }
    }
    Write-Host 'Dependency extraction retry and artifact identity checks passed.'
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
