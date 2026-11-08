param([string]$Label='direct2d-integration')
$ErrorActionPreference='Stop'
$pluginDirectory=Split-Path $PSScriptRoot
$resultDirectory=Join-Path (Get-Location) ('work/'+$Label)
New-Item -ItemType Directory -Force $resultDirectory | Out-Null
$env:MINILYRICS_SETTINGS=Join-Path $resultDirectory 'settings.json'
$capture=Join-Path $resultDirectory 'capture.png'
$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
$native=Start-Process -FilePath (Join-Path $pluginDirectory 'MiniLyricsHost.exe') -ArgumentList @('--port',$port,'--token','0123456789abcdef0123456789abcdef','--capture',('"'+$capture+'"')) -WindowStyle Hidden -PassThru
$null=$native.Handle
try {
 & node (Join-Path $PSScriptRoot 'direct2d-integration.cjs') $port $capture
 if($LASTEXITCODE -ne 0){throw 'Direct2D integration failed'}
 if(!$native.WaitForExit(5000)){throw 'Direct2D host did not exit cleanly'}
} finally {if(!$native.HasExited){Stop-Process -Id $native.Id -Force}}
