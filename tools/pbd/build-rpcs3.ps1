# Builds Release x64 of this repository with the dependencies that setup-dependencies.ps1 restores. -CheckOnly checks
# the source, the dependencies and the toolchain, prints what the build would use, and builds nothing.
param(
    [ValidateRange(1, 32)]
    [int]$Jobs = 4,
    [string]$VulkanSdk,
    [string]$VisualStudioPath,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = $repository
$executable = Join-Path $source 'bin\rpcs3.exe'
# The build replaces bin\rpcs3.exe, which Windows keeps locked while that copy runs.
$running = @(Get-Process -Name rpcs3 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $executable })
if (-not $CheckOnly -and $running.Count -ne 0) { throw "Close the running emulator first: $executable" }
. (Join-Path $PSScriptRoot 'storage-guard.ps1')
if (-not $CheckOnly) { Assert-PbdStorage $repository 'build' }
$deps = Join-Path $repository 'build\deps'
if (-not $VulkanSdk) { $VulkanSdk = Join-Path $deps 'VulkanSDK\1.4.341.1' }
$lock = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') | ConvertFrom-Json
$qt = Join-Path $deps 'Qt\6.11.2\msvc2022_64'
$setupHint = 'run tools\pbd\setup-dependencies.ps1'
$googleTest = 'Microsoft.googletest.v140.windesktop.msvcstl.static.rt-static'

function Assert-File([string]$path, [string]$description) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing ${description}: $path"
    }
}

# The log folder keeps the exact source a build used, uncommitted and untracked files included.
function Save-UntrackedSource([string]$logDirectory) {
    $files = @(& git -C $source -c core.quotepath=false ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list untracked source files' }
    $records = @()
    foreach ($relative in $files) {
        $sourceFile = [IO.Path]::GetFullPath((Join-Path $source $relative))
        if (-not $sourceFile.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Untracked file resolved outside the repository: $relative"
        }
        $copy = Join-Path (Join-Path $logDirectory 'untracked-source') $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $copy) | Out-Null
        Copy-Item -LiteralPath $sourceFile -Destination $copy
        $records += [ordered]@{
            relative_path = $relative
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $copy).Hash.ToLowerInvariant()
            bytes = (Get-Item -LiteralPath $copy).Length
        }
    }
    $manifest = Join-Path $logDirectory 'untracked-source.json'
    ConvertTo-Json -InputObject @($records) -Depth 3 | Set-Content -Encoding UTF8 -LiteralPath $manifest
}

Assert-File (Join-Path $source 'rpcs3.sln') 'RPCS3 source solution'
$commit = (& git -C $source rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine the repository HEAD' }
& git -C $source merge-base --is-ancestor $lock.rpcs3_commit HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Expected RPCS3 base $($lock.rpcs3_commit); found unrelated HEAD $commit"
}
$dependencyDefinitions = @('.gitmodules', 'llvm.lock', '3rdparty', '.ci/setup-windows.sh', '.github/workflows/rpcs3.yml')
& git -C $source diff --quiet $lock.rpcs3_commit -- @dependencyDefinitions
if ($LASTEXITCODE -ne 0) {
    throw 'RPCS3 dependency definitions differ from the lock base; review and update the dependency lock first'
}
$qtFiles = @('bin\moc.exe', 'bin\uic.exe', 'bin\windeployqt.exe', 'include\QtCore\qglobal.h', 'lib\Qt6Core.lib',
    'lib\Qt6Multimedia.lib', 'lib\Qt6Svg.lib')
foreach ($relative in $qtFiles) {
    Assert-File (Join-Path $qt $relative) "Qt dependency; $setupHint"
}
Assert-File (Join-Path $source 'build\lib_ext\Release-x64\llvm_build\lib\LLVMCore.lib') "prebuilt LLVM; $setupHint"
$googleTestTargets = Join-Path $source "packages\$googleTest.1.8.1.8\build\native\$googleTest.targets"
Assert-File $googleTestTargets "GoogleTest NuGet dependency; $setupHint"
Assert-File (Join-Path $VulkanSdk 'Include\vulkan\vulkan.h') 'Vulkan 1.4.341.1 headers; install the SDK or pass -VulkanSdk'
Assert-File (Join-Path $VulkanSdk 'Lib\vulkan-1.lib') 'Vulkan x64 import library'
$submodules = @(& git -C $source submodule status)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the submodule state' }
# LLVM comes prebuilt, and FAudio and Feral GameMode are not part of the Windows build, so those may stay uninitialized.
$optional = '3rdparty/(llvm/llvm|FAudio|feralinteractive/feralinteractive)(\s|$)'
$invalidSubmodules = @($submodules | Where-Object { $_ -match '^[+U]' -or ($_ -match '^-' -and $_ -notmatch $optional) })
if ($invalidSubmodules.Count -gt 0) {
    throw "Missing or mismatched submodules; ${setupHint}:`n$($invalidSubmodules -join "`n")"
}

if (-not $VisualStudioPath) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    Assert-File $locator 'Visual Studio locator'
    $VisualStudioPath = (& $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or -not $VisualStudioPath) {
        throw 'No complete Visual Studio installation with C++ x64 tools was found'
    }
}
$devCmd = Join-Path $VisualStudioPath 'Common7\Tools\VsDevCmd.bat'
$msbuild = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$cmakeDirectory = Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDirectory = Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
Assert-File $devCmd 'Visual Studio developer environment'
Assert-File $msbuild 'x64 MSBuild'
Assert-File (Join-Path $cmakeDirectory 'cmake.exe') 'Visual Studio CMake component'
Assert-File (Join-Path $ninjaDirectory 'ninja.exe') 'Visual Studio Ninja component'

$previousEnvironment = [Environment]::GetEnvironmentVariables('Process')
$buildExitCode = 1
try {
    # Import the developer environment in memory, restoring it when this script exits.
    $environmentLines = & $env:ComSpec /d /c ('call "{0}" -arch=x64 -host_arch=x64 >nul && set' -f $devCmd)
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed' }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            [Environment]::SetEnvironmentVariable($name, $line.Substring($separator + 1), 'Process')
        }
    }
    $env:QTDIR = $qt
    $env:VULKAN_SDK = [IO.Path]::GetFullPath($VulkanSdk)
    $env:PATH = "$cmakeDirectory;$ninjaDirectory;$($env:PATH)"
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    $pythonVersion = (& $python --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $pythonVersion -notmatch '^Python 3\.') {
        throw 'A working Python 3 installation must be on PATH'
    }
    $cmakeOutput = @(& (Join-Path $cmakeDirectory 'cmake.exe') --version)
    if ($LASTEXITCODE -ne 0) { throw 'CMake cannot run' }
    $cmakeVersion = $cmakeOutput[0]
    $ninjaOutput = @(& (Join-Path $ninjaDirectory 'ninja.exe') --version)
    if ($LASTEXITCODE -ne 0) { throw 'Ninja cannot run' }
    $ninjaVersion = $ninjaOutput[0]
    $preflight = [ordered]@{
        source = $source
        base_commit = $lock.rpcs3_commit
        source_commit = $commit
        visual_studio = $VisualStudioPath
        compiler = $compiler
        qt = $env:QTDIR
        vulkan_sdk = $env:VULKAN_SDK
        cmake = $cmakeVersion
        ninja = $ninjaVersion
        python = $pythonVersion
        configuration = 'Release'
        platform = 'x64'
        parallel_projects = $Jobs
        ninja_jobs_per_project = 1
    }
    if ($CheckOnly) {
        [pscustomobject]$preflight
        return
    }

    $started = [DateTimeOffset]::Now
    $logDirectory = Join-Path $repository ('build\pbd-logs\' + $started.ToString('yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $preflight | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $logDirectory 'environment.json')
    $sourcePatch = Join-Path $logDirectory 'source.patch'
    & git -C $source diff HEAD --binary | Set-Content -Encoding UTF8 -LiteralPath $sourcePatch
    Save-UntrackedSource $logDirectory
    # Some upstream projects call plain ninja, which otherwise ignores MSBuild's
    # parallelism limit. Cap those invocations without changing upstream files.
    $parallelTargets = Join-Path $logDirectory 'bounded-build.targets'
    @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Target Name="PbdBoundNinja" BeforeTargets="Build" Condition="'$(ConfigurationType)' == 'Makefile'">
    <PropertyGroup>
      <NMakeBuildCommandLine>$(NMakeBuildCommandLine.Replace('ninja', 'ninja -j1'))</NMakeBuildCommandLine>
    </PropertyGroup>
  </Target>
</Project>
'@ | Set-Content -Encoding UTF8 -LiteralPath $parallelTargets
    $arguments = @(
        (Join-Path $source 'rpcs3.sln'),
        '/nologo', '/nr:false', "/m:$Jobs", '/v:minimal', '/clp:ErrorsOnly;Summary',
        '/p:Configuration=Release', '/p:Platform=x64', '/p:PreferredToolArchitecture=x64',
        "/p:CL_MPCount=$Jobs", '/p:UseMultiToolTask=true',
        "/p:MultiProcMaxCount=$Jobs", '/p:EnforceProcessCountAcrossBuilds=true',
        ('/p:CustomAfterMicrosoftCommonTargets=' + $parallelTargets),
        ('/flp:logfile=' + (Join-Path $logDirectory 'msbuild.log') + ';verbosity=normal'),
        ('/bl:' + (Join-Path $logDirectory 'msbuild.binlog'))
    )
    Write-Host "Building RPCS3 Release x64 with up to $Jobs parallel projects. Logs: $logDirectory"
    # __FILE__ in assertions and log lines would carry this PC's source folder (and the user name in it) into the
    # executable and every emulator log; cl.exe trims that prefix from every such path. CL is read by every cl.exe the
    # build starts, the third-party projects' own builds included (the whole environment is restored below). Objects
    # compiled before this switch keep the full path until they are rebuilt, so a build that must be free of it is a
    # full rebuild.
    $env:CL = '"/d1trimfile:' + [IO.Path]::GetFullPath($source).TrimEnd('\') + '\\"'
    Push-Location $source
    try {
        & $msbuild @arguments 2>&1 | Tee-Object -FilePath (Join-Path $logDirectory 'console.log')
        $buildExitCode = $LASTEXITCODE
    } finally {
        Pop-Location
        $ended = [DateTimeOffset]::Now
        [ordered]@{
            started = $started.ToString('o')
            ended = $ended.ToString('o')
            elapsed_seconds = [math]::Round(($ended - $started).TotalSeconds, 3)
            exit_code = $buildExitCode
            executable = $executable
        } | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $logDirectory 'result.json')
    }
    if ($buildExitCode -eq 0) {
        Assert-File $executable 'built RPCS3 executable'
        Write-Host "Build succeeded. Executable: $executable"
    } else {
        Write-Host "Build failed with exit code $buildExitCode. See $(Join-Path $logDirectory 'msbuild.log')"
    }
} finally {
    foreach ($name in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        if (-not $previousEnvironment.Contains($name)) {
            [Environment]::SetEnvironmentVariable($name, $null, 'Process')
        }
    }
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
}
exit $buildExitCode
