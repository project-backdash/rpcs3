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
    & git -C $testRoot -c user.name='Dependency fixture' -c user.email=fixture@localhost commit --allow-empty --quiet -m 'Dependency fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot commit the dependency fixture' }
    $baseCommit = (& git -C $testRoot rev-parse HEAD).Trim()

    Set-Content -LiteralPath (Join-Path $payload 'existing.txt') -Value 'complete' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $payload 'missing.txt') -Value 'restored' -Encoding UTF8
    $archive = Join-Path $downloads 'googletest.zip'
    [System.IO.Compression.ZipFile]::CreateFromDirectory($payload, $archive)
    # This artifact is verified and copied; ZIP extraction does not invoke it.
    $sevenArchive = Join-Path $downloads '7zr-fixture.ps1'
    Set-Content -LiteralPath $sevenArchive -Value "throw 'ZIP and Vulkan dependencies require their own extraction paths'" -Encoding UTF8
    $artifact = [ordered]@{
        name = 'googletest'
        version = '1'
        url = 'https://invalid.local/googletest.zip'
        sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        destination = 'packages/googletest'
    }
    $ccachePayload = Join-Path $testRoot 'ccache-payload\ccache-4.12.3-windows-x86_64'
    New-Item -ItemType Directory -Force -Path $ccachePayload | Out-Null
    Set-Content -LiteralPath (Join-Path $ccachePayload 'ccache.exe') -Value 'compiler wrapper' -Encoding UTF8
    $ccacheArchive = Join-Path $downloads 'ccache.zip'
    [System.IO.Compression.ZipFile]::CreateFromDirectory((Split-Path -Parent $ccachePayload), $ccacheArchive)
    $vulkanArchive = Join-Path $downloads 'vulkan-fixture.ps1'
    @'
$ErrorActionPreference = 'Stop'
$rootIndex = [Array]::IndexOf($args, '--root')
if ($rootIndex -lt 0 -or $args -notcontains 'copy_only=1' -or
    $args -notcontains '--confirm-command' -or $args -notcontains 'install' -or
    $args -notcontains '--accept-licenses' -or $args -notcontains '--default-answer' -or
    $args -notcontains '--max-concurrent-operations' -or $args -notcontains '--cache-path') {
    throw 'Vulkan restoration must use the unattended copy-only installer with bounded extraction'
}
$destination = $args[$rootIndex + 1]
foreach ($relative in @('Include\vulkan\vulkan.h', 'Lib\vulkan-1.lib')) {
    $file = Join-Path $destination $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    Set-Content -LiteralPath $file -Value 'SDK fixture'
}
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'fail-vulkan')) { exit 7 }
exit 0
'@ | Set-Content -LiteralPath $vulkanArchive -Encoding UTF8
    $ccacheArtifact = [ordered]@{
        name = 'ccache'
        version = '4.12.3'
        url = 'https://invalid.local/ccache.zip'
        sha256 = (Get-FileHash -LiteralPath $ccacheArchive -Algorithm SHA256).Hash
        destination = 'build/deps/ccache'
    }
    $vulkanArtifact = [ordered]@{
        name = 'vulkansdk'
        version = '1'
        url = 'https://invalid.local/vulkan-fixture.ps1'
        sha256 = (Get-FileHash -LiteralPath $vulkanArchive -Algorithm SHA256).Hash
        destination = 'build/deps/VulkanSDK/1'
    }
    $qtArtifact = [ordered]@{
        name = 'qtbase'
        version = '9.8.7'
        url = $artifact.url
        sha256 = $artifact.sha256
        destination = 'build/deps/Qt/9.8.7/msvc2022_64'
    }
    $manifest = [ordered]@{
        rpcs3_commit = $baseCommit
        artifacts = @(
            [ordered]@{
                name = '7zr'
                version = '1'
                url = 'https://invalid.local/7zr-fixture.ps1'
                sha256 = (Get-FileHash -LiteralPath $sevenArchive -Algorithm SHA256).Hash
                destination = 'build/deps/7zr.ps1'
            },
            $artifact,
            $ccacheArtifact,
            $vulkanArtifact,
            $qtArtifact
        )
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $scripts 'dependencies.lock.json') -Encoding UTF8
    $destination = Join-Path $testRoot $artifact.destination
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Set-Content -LiteralPath (Join-Path $destination 'existing.txt') -Value 'partial' -Encoding UTF8
    $unrelated = Join-Path $testRoot 'packages\unrelated.txt'
    Set-Content -LiteralPath $unrelated -Value 'keep' -Encoding UTF8

    $setupOutput = & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules 6>&1 | Out-String -Width 4096
    if (-not $setupOutput.Contains("QTDIR=$(Join-Path $testRoot $qtArtifact.destination)")) {
        throw 'Setup did not report the Qt destination from the lock'
    }
    foreach ($file in @('ccache.exe', 'cl.exe')) {
        $wrapper = Join-Path (Join-Path $testRoot $ccacheArtifact.destination) $file
        if ((Get-Content -Raw -LiteralPath $wrapper).Trim() -cne 'compiler wrapper') {
            throw "ccache did not arrange $file beside the compiler wrapper"
        }
    }
    foreach ($relative in @('Include\vulkan\vulkan.h', 'Lib\vulkan-1.lib')) {
        if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $testRoot $vulkanArtifact.destination) $relative))) {
            throw "Vulkan restoration is missing $relative"
        }
    }
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
    $qtArtifact.sha256 = $artifact.sha256
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
    $vulkanMarker = Join-Path $testRoot 'build\deps\vulkansdk.restored'
    Remove-Item -LiteralPath $vulkanMarker
    Set-Content -LiteralPath (Join-Path $downloads 'fail-vulkan') -Value 'fail'
    $failure = $null
    try {
        & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules
    } catch {
        $failure = $_
    }
    if (-not $failure -or $failure.ToString() -notmatch 'Extraction failed: vulkansdk' -or
        (Test-Path -LiteralPath $vulkanMarker)) {
        throw 'Failed Vulkan extraction did not preserve its failure and withhold its marker'
    }
    Remove-Item -LiteralPath (Join-Path $downloads 'fail-vulkan')
    & (Join-Path $scripts 'setup-dependencies.ps1') -SkipSubmodules
    if (-not (Test-Path -LiteralPath $vulkanMarker)) { throw 'Vulkan restoration could not resume after failure' }

    $parseErrors = $null
    $tokens = $null
    $build = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'pbd\build-rpcs3.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw 'Cannot parse the build script' }
    $qtAssignment = $build.Find({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ieq 'qt'
    }, $true)
    $lock = [pscustomobject]$manifest
    $repository = $testRoot
    . ([scriptblock]::Create($qtAssignment.Extent.Text))
    if ($qt -cne (Join-Path $testRoot $qtArtifact.destination)) { throw 'Build does not use the locked Qt destination' }
    $vulkanDefault = $build.Find({
        param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '-not $VulkanSdk'
    }, $true)
    $VulkanSdk = ''
    . ([scriptblock]::Create($vulkanDefault.Extent.Text))
    if ($VulkanSdk -cne (Join-Path $testRoot $vulkanArtifact.destination)) {
        throw 'Build does not use the locked Vulkan destination'
    }
    $VulkanSdk = 'C:\external-sdk'
    . ([scriptblock]::Create($vulkanDefault.Extent.Text))
    if ($VulkanSdk -cne 'C:\external-sdk') { throw 'Build did not preserve the explicit Vulkan SDK override' }

    $setup = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'pbd\setup-dependencies.ps1'), [ref]$tokens, [ref]$parseErrors)
    $sevenCommand = $setup.Find({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.Extent.Text -like '& $seven x *'
    }, $true)
    $submoduleCommand = $setup.Find({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.Extent.Text -like '* submodule update *'
    }, $true)
    if ($sevenCommand.Extent.Text -notmatch '-mmt1' -or $submoduleCommand.Extent.Text -notmatch '--jobs=1') {
        throw 'Archive extraction and submodule restoration must use one worker'
    }
    $hashFunction = $setup.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-ArtifactHash'
    }, $true)
    . ([scriptblock]::Create($hashFunction.Extent.Text))
    $sizedArtifact = [pscustomobject]@{ sha256 = $artifact.sha256; bytes = (Get-Item -LiteralPath $archive).Length }
    if (-not (Test-ArtifactHash $archive $sizedArtifact)) { throw 'Measured archive was rejected' }
    $sizedArtifact.bytes++
    if (Test-ArtifactHash $archive $sizedArtifact) { throw 'A mismatched archive byte count was accepted' }
    Write-Host 'Locked Qt paths, ccache ZIP layout and copy-only Vulkan success/failure checks passed.'
    Write-Host 'Dependency extraction retry and artifact identity checks passed.'
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
