param(
    # The restored googletest package folder (packages\Microsoft.googletest.v140...) of a dependency-complete checkout.
    [Parameter(Mandatory)][string]$Dependencies,
    [string]$Output = (Join-Path $env:TEMP 'pbd-spectator-native-tests')
)
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x64 is required.' }
$devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$lines = & $env:ComSpec /d /c ('call "{0}" -arch=x64 -host_arch=x64 >nul && set' -f $devCmd)
if ($LASTEXITCODE) { throw 'MSVC environment setup failed.' }
foreach ($line in $lines) {
    $at = $line.IndexOf('=')
    if ($at -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$at), $line.Substring($at+1), 'Process') }
}
[IO.Directory]::CreateDirectory($Output) | Out-Null
$lib = Join-Path $Dependencies 'lib\native\v140\windesktop\msvcstl\static\rt-static\x64\Release'
$tests = @('spectator_channel', 'spectator_standby', 'spectator_ui', 'spectator_parser_audit', 'spectator_packet_random', 'vs_cards')
$files = @($tests | ForEach-Object { Join-Path $source "rpcs3\tests\pbd\${_}_test.cpp" }) + @((Join-Path $source 'rpcs3\tests\test.cpp'))
Push-Location $Output
try {
    & cl.exe /nologo /std:c++20 /EHsc /MT /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "/I$source\rpcs3" "/I$Dependencies\build\native\include" @files (Join-Path $lib 'gtest.lib') /Fe:spectator-tests.exe 2>&1 | Tee-Object -FilePath compile.log
    if ($LASTEXITCODE) { throw 'Native spectator test compilation failed.' }
    & .\spectator-tests.exe '--gtest_output=xml:results.xml' 2>&1 | Tee-Object -FilePath test.log
    if ($LASTEXITCODE) { throw 'Native spectator tests failed.' }
}
finally { Pop-Location }
