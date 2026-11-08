param(
 [string]$PluginDirectory = (Split-Path $PSScriptRoot),
 [string]$Label = 'optimized',
 [switch]$Baseline
)
$ErrorActionPreference = 'Stop'
$workspace=(Get-Location).Path
$resultDirectory=Join-Path $workspace ('work/render-results-'+$Label)
New-Item -ItemType Directory -Force $resultDirectory | Out-Null
$env:MINILYRICS_SETTINGS=Join-Path $resultDirectory 'settings.json'
$capture=Join-Path $resultDirectory 'capture.png'
$result=Join-Path $resultDirectory 'result.json'
$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
$native=Start-Process -FilePath (Join-Path $PluginDirectory 'MiniLyricsHost.exe') -ArgumentList @('--port',$port,'--token','0123456789abcdef0123456789abcdef','--capture',('"'+$capture+'"')) -WindowStyle Hidden -PassThru
try {
 $mode=if($Baseline){'baseline'}else{'optimized'}
 & node (Join-Path $PSScriptRoot 'render-benchmark.cjs') $port $result $capture $mode
 if($LASTEXITCODE -ne 0){throw 'Native rendering assertions failed'}
 if(!$native.WaitForExit(5000)){throw 'Native rendering test did not exit cleanly'}
 Get-Content -Raw -LiteralPath $result
} finally {
 if(!$native.HasExited){Stop-Process -Id $native.Id -Force}
}
