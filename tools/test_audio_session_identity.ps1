param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$taskRepository = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path $taskRepository 'tmp\audio-session'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskExecutable = Join-Path $taskOutput 'identity.exe'
$taskObject = Join-Path $taskOutput 'identity.obj'
& $Compiler /nologo /std:c++20 /EHsc /W4 (Join-Path $PSScriptRoot 'audio_session_identity_test.cpp') "/Fo:$taskObject" "/Fe:$taskExecutable" /link ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'Audio session fixture did not compile.' }
& $taskExecutable --self-test
if ($LASTEXITCODE -ne 0) { throw 'Audio session identity checks failed.' }
Write-Output "Silent session fixture: $taskExecutable"
