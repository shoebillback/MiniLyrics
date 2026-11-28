param(
 [switch]$CI,
 [switch]$IncludeHarness
)
$ErrorActionPreference='Stop'
$testsRoot=$PSScriptRoot
$repoRoot=Split-Path $testsRoot -Parent

Write-Output '=== JS unit and integration tests ==='
$jsTests=Get-ChildItem -Path $testsRoot -Filter '*.test.cjs' | Sort-Object Name | ForEach-Object { $_.FullName }
& node --test $jsTests
if($LASTEXITCODE -ne 0){throw 'Node.js test suite failed'}

Write-Output '=== Standalone C++ unit tests ==='
$unitDir=Join-Path $repoRoot 'work/unit-tests'
New-Item -ItemType Directory -Force -Path $unitDir | Out-Null
$standalone=@('json.test.cpp','cover-geometry.test.cpp','transport-geometry.test.cpp','lyric-motion.test.cpp','window-mode.test.cpp')
foreach($src in $standalone){
 $exe=Join-Path $unitDir ([IO.Path]::ChangeExtension($src,'.exe'))
 & g++ -std=c++17 -Os -Wall -Wextra -Wno-misleading-indentation -DUNICODE -D_UNICODE (Join-Path $testsRoot $src) -o $exe -luser32 -lgdi32
 if($LASTEXITCODE -ne 0){throw "Compilation failed for $src"}
 & $exe
 if($LASTEXITCODE -ne 0){throw "Test failed for $src"}
}

Write-Output '=== Native host regression suites ==='
if($CI){
 & (Join-Path $testsRoot 'native.ps1') -Focus ci
}else{
 & (Join-Path $testsRoot 'native.ps1') -Focus all
 & (Join-Path $testsRoot 'native.ps1') -Focus background-edge
 & (Join-Path $testsRoot 'native.ps1') -Focus cover
}

if($IncludeHarness){
 Write-Output '=== Interactive host harness suite ==='
 & (Join-Path $testsRoot 'direct2d.ps1')
 if($LASTEXITCODE -ne 0){throw 'Harness failed for direct2d.ps1'}
}
