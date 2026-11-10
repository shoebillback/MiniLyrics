param(
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
 [string]$Repository = 'shoebillback/MiniLyrics',
 [string]$BuildUrl = ''
)
$ErrorActionPreference='Stop'
$sourceRoot=Split-Path $PSScriptRoot -Parent
$target=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $target){throw 'Use a new empty staging path.'}
New-Item -ItemType Directory -Path $target | Out-Null
$files=@('manifest.json','main.js','MiniLyricsHost.exe','preview.png','LICENSE','THIRD_PARTY.md')
foreach($file in $files){Copy-Item -LiteralPath (Join-Path $sourceRoot $file) -Destination (Join-Path $target $file)}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'licenses') -Destination (Join-Path $target 'licenses') -Recurse
$sourceInfo=@(
 'MiniLyrics — GPL-3.0-or-later',
 'Author: shoebillback',
 ('Corresponding source commit: '+$SourceCommit),
 ('Source: https://github.com/'+$Repository+'/tree/'+$SourceCommit),
 ('Full source archive: https://github.com/'+$Repository+'/archive/'+$SourceCommit+'.zip'),
 'Build: install MinGW-w64 g++ and run powershell -NoProfile -File .\build.ps1',
 ('GitHub Actions build: '+$BuildUrl),
 'Keep the referenced source commit and its complete build scripts publicly available.'
)
[IO.File]::WriteAllLines((Join-Path $target 'SOURCE.txt'),$sourceInfo,[Text.UTF8Encoding]::new($false))
$hashes=Get-ChildItem -LiteralPath $target -File -Recurse | Sort-Object FullName | ForEach-Object {
 $relative=$_.FullName.Substring($target.Length+1).Replace('\','/')
 (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+$relative
}
[IO.File]::WriteAllLines((Join-Path $target 'SHA256SUMS.txt'),[string[]]$hashes,[Text.UTF8Encoding]::new($false))
Write-Output $target
