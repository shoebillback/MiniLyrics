param([string]$ResultDirectory = (Join-Path (Get-Location) 'work/native-host-regression'), [ValidateSet('all','font','flight','background','optimization','background-edge','resize','cover','dpi','words','moving','prelude','cleanup')][string]$Focus='all')
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $ResultDirectory | Out-Null
$resultPath = (Resolve-Path -LiteralPath $ResultDirectory).Path
$sourceName = if($Focus -eq 'background-edge'){'background-edge.test.cpp'}elseif($Focus -eq 'cover'){'cover-resolution.test.cpp'}elseif($Focus -eq 'dpi'){'dpi.test.cpp'}else{'native-host.test.cpp'}
$testExecutable = Join-Path $resultPath ([IO.Path]::ChangeExtension($sourceName,'.exe'))
$nativeArgs = @('-std=c++17','-Os','-static','-DUNICODE','-D_UNICODE',(Join-Path $PSScriptRoot $sourceName),'-o',$testExecutable,
 '-ld3d11','-ld3dcompiler','-ldxgi','-ld2d1','-ldwrite','-lwindowscodecs','-lwinhttp','-lws2_32','-lbcrypt','-lcrypt32','-lole32','-luuid','-lshell32','-lgdi32')
& g++ @nativeArgs
if($LASTEXITCODE -ne 0){throw 'Native regression compilation failed'}
$previousSettings = $env:MINILYRICS_SETTINGS
try {
 $env:MINILYRICS_SETTINGS = Join-Path $resultPath 'unused-settings.json'
 if($Focus -eq 'cover'){& node (Join-Path $PSScriptRoot 'cover-resolution.cjs') $testExecutable $resultPath}
 elseif($Focus -eq 'background-edge'){& $testExecutable (Join-Path $resultPath 'edge-results.json')}
 else{& $testExecutable $resultPath $Focus}
 if($LASTEXITCODE -ne 0){throw 'Native regression failed'}
} finally { $env:MINILYRICS_SETTINGS = $previousSettings }
