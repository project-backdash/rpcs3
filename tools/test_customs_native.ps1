param(
    # The restored googletest package folder (packages\Microsoft.googletest.v140...) and the dependency-complete
    # checkout whose build tree holds the libraries these tests link.
    [Parameter(Mandatory)][string]$Dependencies,
    [Parameter(Mandatory)][string]$NativeDependencies,
    [string]$Output = (Join-Path $env:TEMP 'pbd-customs-native-tests')
)
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x64 is required.' }
$env:Path = (Split-Path -Parent $locator) + [IO.Path]::PathSeparator + $env:Path
$devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$lines = & $env:ComSpec /d /c ('call "{0}" -arch=x64 -host_arch=x64 >nul && set' -f $devCmd)
if ($LASTEXITCODE) { throw 'MSVC environment setup failed.' }
foreach ($line in $lines) {
    $at = $line.IndexOf('=')
    if ($at -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$at), $line.Substring($at+1), 'Process') }
}
[IO.Directory]::CreateDirectory($Output) | Out-Null
$lib = Join-Path $Dependencies 'lib\native\v140\windesktop\msvcstl\static\rt-static\x64\Release'
$tests = @('custom_online', 'custom_look', 'costume_select', 'peer_message', 'peer_message_conformance', 'set_channel', 'spectator_channel', 'replay_game_file_parser_audit')
$files = @($tests | ForEach-Object { Join-Path $source "rpcs3\tests\pbd\${_}_test.cpp" }) + @((Join-Path $source "rpcs3\tests\test.cpp"))
# Compile the production JSON constructor independently of its guest-memory operations.
$restore = Get-Content -LiteralPath (Join-Path $source 'rpcs3\Emu\PBD\pbd_replay_restore.cpp') -Raw
$begin = $restore.IndexOf('generators::generators(const pbd::json::json_value& value)')
$end = $restore.IndexOf('bool generators::apply', $begin)
$constructor = "#include `"Emu/PBD/pbd_replay_restore.h`"`n#include `"Emu/Cell/pbd_replay_game_file.h`"`nnamespace pbd::replay_restore { using namespace pbd::replay_game_file;`n" + $restore.Substring($begin, $end - $begin) + "`n}`n"
$constructorPath = Join-Path $Output 'restore-json.cpp'
[IO.File]::WriteAllText($constructorPath, $constructor)
$files += $constructorPath
$photoPath = Join-Path $Output 'custom-photo.cpp'
[IO.File]::WriteAllText($photoPath, (Get-Content -LiteralPath (Join-Path $source 'rpcs3\Emu\PBD\pbd_custom_photo.cpp') -Raw).Replace('#include "stdafx.h"', ''))
$decoderPath = Join-Path $Output 'image-decode.cpp'
[IO.File]::WriteAllText($decoderPath, "#define STB_IMAGE_IMPLEMENTATION`n#include <stb_image.h>`n")
$files += @($photoPath, $decoderPath)
$libraries = @((Join-Path $lib 'gtest.lib'), (Join-Path $lib 'gtest_main.lib'), (Join-Path $NativeDependencies 'build\lib\Release-x64\libpng16.lib'), (Join-Path $NativeDependencies 'build\lib\Release-x64\zlib.lib'))
Push-Location $Output
try {
    & cl.exe /nologo /std:c++20 /EHsc /MT /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "/I$source\rpcs3" "/I$source\rpcs3\Emu\PBD" "/I$NativeDependencies\3rdparty\libpng\libpng" "/I$NativeDependencies\3rdparty\stblib\stb" "/I$Dependencies\build\native\include" @files @libraries /Fe:customs-tests.exe 2>&1 | Tee-Object -FilePath compile.log
    if ($LASTEXITCODE) { throw 'Native customs test compilation failed.' }
    & .\customs-tests.exe '--gtest_output=xml:results.xml' 2>&1 | Tee-Object -FilePath test.log
    if ($LASTEXITCODE) { throw 'Native customs tests failed.' }
}
finally { Pop-Location }
