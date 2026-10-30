param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$testRoot = [IO.Path]::GetFullPath((Join-Path $repository ('build\compiler-arguments-' + [Guid]::NewGuid().ToString('N'))))
if (-not $testRoot.StartsWith($repository + '\build\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Test directory is outside the build folder: $testRoot"
}

$parseErrors = $null
$tokens = $null
$build = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'pbd\build-rpcs3.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Cannot parse the build script' }
$assignment = $build.Find({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ieq 'env:CL'
}, $true)
if (-not $assignment) { throw 'The build script does not set compiler options' }
$setCompilerOptions = [scriptblock]::Create($assignment.Extent.Text)
$previousCl = $env:CL
$previousClSuffix = $env:_CL_

try {
    $env:_CL_ = $null
    foreach ($name in @('plain', 'with spaces')) {
        $source = Join-Path $testRoot $name
        New-Item -ItemType Directory -Path $source -Force | Out-Null
        $inputFile = Join-Path $source 'file.c'
        Set-Content -LiteralPath $inputFile -Value '__FILE__' -Encoding ascii
        # Evaluate the build's assignment; cl.exe parses CL and exposes the resulting trim through __FILE__.
        . $setCompilerOptions
        $diagnostics = Join-Path $testRoot 'compiler.err'
        $output = @(& $Compiler /nologo /EP /TC $inputFile 2> $diagnostics)
        if ($LASTEXITCODE -ne 0 -or ($output -join "`n").Trim() -cne '"file.c"') {
            throw "Compiler path trimming failed for ${source}: $($output -join "`n") $(Get-Content -Raw -LiteralPath $diagnostics)"
        }
        Set-Content -LiteralPath $inputFile -Value 'int answer(void) { return 42; }' -Encoding ascii
        $object = Join-Path $source 'file.obj'
        & $Compiler /nologo /c /TC "/Fo$object" $inputFile 2> $diagnostics | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $object)) {
            throw "Compiler did not produce the object for ${source}: $(Get-Content -Raw -LiteralPath $diagnostics)"
        }
    }
    Write-Host 'Compiler path trimming passed for plain paths and paths with spaces.'
} finally {
    $env:CL = $previousCl
    $env:_CL_ = $previousClSuffix
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
