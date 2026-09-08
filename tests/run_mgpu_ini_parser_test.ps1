$ErrorActionPreference = 'Stop'

# Standalone runner: it uses only this candidate's source/tests/assets, always
# creates a fresh output directory, always compiles, and never accepts a stale
# executable as a green result.
$testRoot = $PSScriptRoot
$root = Split-Path -Parent $testRoot
$asset = Join-Path $root 'assets\mgpu.ini'
$cpp = Join-Path $testRoot 'mgpu_ini_parser_test.cpp'
$testScratch = Join-Path $testRoot ('.test-tmp-' + [guid]::NewGuid().ToString('N'))
$buildDir = $testScratch
$out = Join-Path $buildDir 'mgpu_ini_parser_test.exe'
$obj = Join-Path $buildDir 'mgpu_ini_parser_test.obj'

try {
    if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
        throw "public shipped config is missing: $asset"
    }
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    $clCommand = Get-Command cl.exe -ErrorAction SilentlyContinue
    $compileExit = $null
    if ($null -ne $clCommand) {
        Push-Location $buildDir
        try {
            & $clCommand.Source /nologo /std:c++17 /EHsc /W4 `
                "/I$root\src" "/Fo$obj" "/Fe$out" $cpp
            $compileExit = $LASTEXITCODE
        }
        finally { Pop-Location }
    }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            throw 'MSVC cl.exe was not available and vswhere.exe was not found.'
        }
        $install = (& $vswhere -latest -products '*' -property installationPath | Select-Object -First 1).Trim()
        $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
            throw "MSVC vcvars64.bat was not found: $vcvars"
        }
        $compileLine = 'call "' + $vcvars + '" && cl.exe /nologo /std:c++17 /EHsc /W4 ' +
            '/I"' + (Join-Path $root 'src') + '" /Fo"' + $obj + '" /Fe"' + $out + '" "' + $cpp + '"'
        Push-Location $buildDir
        try {
            cmd.exe /d /s /c $compileLine
            $compileExit = $LASTEXITCODE
        }
        finally { Pop-Location }
    }
    if ($compileExit -ne 0) {
        throw "focused parser compile failed with exit code $compileExit"
    }
    if (-not (Test-Path -LiteralPath $out -PathType Leaf)) {
        throw 'focused parser compile reported success but produced no executable'
    }

    & $out
    if ($LASTEXITCODE -ne 0) { throw "parser integration test exited with $LASTEXITCODE" }
    & $out $asset
    if ($LASTEXITCODE -ne 0) { throw "shipped-config parser test exited with $LASTEXITCODE" }
}
finally {
    if (Test-Path -LiteralPath $buildDir) {
        $resolvedTests = [IO.Path]::GetFullPath($testRoot)
        $resolvedBuild = [IO.Path]::GetFullPath($buildDir)
        $buildLeaf = [IO.Path]::GetFileName($resolvedBuild)
        $buildParent = [IO.Path]::GetDirectoryName($resolvedBuild)
        $safeLeaf = $buildLeaf -match '^\.test-tmp-[0-9a-f]{32}$'
        $safeParent = $buildParent.Equals($resolvedTests, [StringComparison]::OrdinalIgnoreCase)
        if (-not ($safeLeaf -and $safeParent)) {
            throw "refusing to remove unexpected temporary path: $resolvedBuild"
        }
        Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
