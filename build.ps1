param(
 [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist'),
 [switch]$SkipNativeBuild
)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
if (!$SkipNativeBuild) {
 $compiler=Get-Command g++.exe -ErrorAction Stop
 $argsList=@('-std=c++17','-Os','-s','-static','-municode','-mwindows','-Wall','-Wextra','-Wno-misleading-indentation','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DUNICODE','-D_UNICODE',
  (Join-Path $taskRoot 'native/Host.cpp'),'-o',(Join-Path $taskRoot 'MiniLyricsHost.exe'),
  '-ld3d11','-ld3dcompiler','-ldxgi','-ld2d1','-ldwrite','-lwindowscodecs','-lwinhttp','-lws2_32','-lbcrypt','-lcrypt32','-lole32','-luuid','-lshell32','-lgdi32')
 & $compiler.Source $argsList
 if ($LASTEXITCODE -ne 0) { throw 'Direct2D native build failed' }
}
if (!(Test-Path -LiteralPath (Join-Path $taskRoot 'MiniLyricsHost.exe'))) { throw 'Native executable missing' }
$version = ([IO.File]::ReadAllText((Join-Path $taskRoot 'manifest.json')) | ConvertFrom-Json).version
$scripts = @('core.js','player.js','lyrics.js','plugin.js')
$source = $scripts | ForEach-Object { [IO.File]::ReadAllText((Join-Path $taskRoot $_)) }
$bundle = '(function(){' + "`n" + 'const MINILYRICS_VERSION=' + (ConvertTo-Json $version -Compress) + ';' + "`n" + ($source -join "`n") + "`n" + '})();'
[IO.File]::WriteAllText((Join-Path $taskRoot 'main.js'),$bundle,(New-Object Text.UTF8Encoding $false))
$outputDir = $OutputDirectory
New-Item -ItemType Directory -Force $outputDir | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$packagePath = Join-Path $outputDir ('MiniLyrics-' + $version + '.plugin')
if (Test-Path -LiteralPath $packagePath) { Remove-Item -LiteralPath $packagePath }
$zip = [IO.Compression.ZipFile]::Open($packagePath,[IO.Compression.ZipArchiveMode]::Create)
try {
 # Include runtime files and the complete corresponding source for GPL distribution.
 $files = @('manifest.json','main.js','MiniLyricsHost.exe','LICENSE','THIRD_PARTY.md',
  'README.md','CHANGELOG.md','preview.png','build.ps1') + $scripts
 foreach ($directory in @('native','tests','licenses')) {
  $files += Get-ChildItem -LiteralPath (Join-Path $taskRoot $directory) -File -Recurse |
   Where-Object { $_.Extension -in @('.cpp','.hpp','.cjs','.ps1','.md','.txt') } |
   ForEach-Object { $_.FullName.Substring($taskRoot.Length + 1).Replace('\','/') }
 }
 foreach ($file in ($files | Sort-Object -Unique)) {
  [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $taskRoot $file),$file,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
 }

} finally { $zip.Dispose() }
Write-Output $packagePath
