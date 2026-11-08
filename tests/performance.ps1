param(
 [string]$PluginDirectory = (Split-Path $PSScriptRoot),
 [string]$Label = 'direct2d',
 [switch]$Baseline
)
$ErrorActionPreference='Stop'
$resultDirectory=Join-Path (Get-Location) ('work/performance-'+$Label)
New-Item -ItemType Directory -Force $resultDirectory | Out-Null
$env:MINILYRICS_SETTINGS=Join-Path $resultDirectory 'settings.json'
$capture=Join-Path $resultDirectory 'capture.png'
$result=Join-Path $resultDirectory 'result.json'
$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$probe.Start();$port=$probe.LocalEndpoint.Port;$probe.Stop()
$native=Start-Process -FilePath (Join-Path $PluginDirectory 'MiniLyricsHost.exe') -ArgumentList @('--port',$port,'--token','0123456789abcdef0123456789abcdef','--capture',('"'+$capture+'"')) -WindowStyle Hidden -PassThru
$null=$native.Handle
$client=$null
try {
 $mode=if($Baseline){'baseline'}else{'optimized'}
 $arguments=@(('"'+(Join-Path $PSScriptRoot 'render-benchmark.cjs')+'"'),$port,('"'+$result+'"'),('"'+$capture+'"'),$mode)
 $client=Start-Process -FilePath (Get-Command node).Source -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $resultDirectory 'client.log') -RedirectStandardError (Join-Path $resultDirectory 'client.err')
 $null=$client.Handle
 $samples=@();$lastCpu=0;$watch=[Diagnostics.Stopwatch]::StartNew();$lastAt=0
 while($watch.Elapsed.TotalSeconds -lt 30){
  Start-Sleep -Milliseconds 250
  $native.Refresh();$client.Refresh();if($native.HasExited -or $client.HasExited){break}
  $at=$watch.Elapsed.TotalSeconds;$cpu=$native.TotalProcessorTime.TotalSeconds
  $samples += [pscustomobject]@{Seconds=$at;CPUPercentOneCore=100*($cpu-$lastCpu)/($at-$lastAt);WorkingSetMiB=$native.WorkingSet64/1MB;PrivateMiB=$native.PrivateMemorySize64/1MB}
  $lastAt=$at;$lastCpu=$cpu
 }
 $client.WaitForExit(5000) | Out-Null
 if(!$client.HasExited -or $client.ExitCode -ne 0){Get-Content -LiteralPath (Join-Path $resultDirectory 'client.err');throw 'Native comparison test failed'}
 if(!$native.WaitForExit(5000)){throw 'Native test did not exit cleanly'}
 $warm=@($samples | Where-Object Seconds -ge 2)
 $summary=[pscustomobject]@{Label=$Label;Samples=$warm.Count;MeanCPUPercentOneCore=[Math]::Round(($warm | Measure-Object CPUPercentOneCore -Average).Average,2);MeanWorkingSetMiB=[Math]::Round(($warm | Measure-Object WorkingSetMiB -Average).Average,2);MeanPrivateMiB=[Math]::Round(($warm | Measure-Object PrivateMiB -Average).Average,2);PeakWorkingSetMiB=[Math]::Round(($samples | Measure-Object WorkingSetMiB -Maximum).Maximum,2)}
 $samples | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultDirectory 'samples.json') -Encoding utf8
 $summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultDirectory 'summary.json') -Encoding utf8
 $summary | ConvertTo-Json
 Get-Content -Raw -LiteralPath $result
} finally {
 if($client -and !$client.HasExited){Stop-Process -Id $client.Id -Force}
 if(!$native.HasExited){Stop-Process -Id $native.Id -Force}
}
