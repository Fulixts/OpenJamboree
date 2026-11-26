# Builds the Recomp Launcher release zip: dist/OpenJamboree-v<version>-Windows-x64.zip
# Contents: the tracked source (git ls-files), recomplauncher.json at the top, the embedded Python
# (python/, from nuget, hash-checked by setup.py --python) and tools/nsp_extract.exe. No game files, keys or GCC.
#   powershell -File launcher/make_release.ps1 -Version 0.1.0
param([Parameter(Mandatory)][string]$Version, [string]$Dotnet = 'dotnet')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path 'python\python.exe')) { & python setup.py --python --deps-only; if ($LASTEXITCODE) { throw 'setup.py failed' } }

& $Dotnet publish tools\nsp_extract -c Release -r win-x64 -o tools\nsp_extract\out -v q
if ($LASTEXITCODE) { throw 'nsp_extract build failed' }

$stage = Join-Path $root "dist\OpenJamboree-v$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in (git ls-files)) {
    $dst = Join-Path $stage $f
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $f $dst
}
Copy-Item -Recurse python (Join-Path $stage 'python')
Get-ChildItem (Join-Path $stage 'python') -Recurse -Directory -Filter __pycache__ | Remove-Item -Recurse -Force
Copy-Item tools\nsp_extract\out\nsp_extract.exe (Join-Path $stage 'tools\nsp_extract.exe')
# SDL3 (zlib) for PlayStation / Xbox pads, with its license
Copy-Item tools\sdl3\SDL3.dll (Join-Path $stage 'tools\SDL3.dll')
Copy-Item tools\sdl3\LICENSE.txt (Join-Path $stage 'tools\SDL3-LICENSE.txt')

# nothing from the game may be in the zip
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.nsp','.nca','.xci','.nso','.nro','.keys' -or $_.Name -in 'main','main.npdm' }
if ($bad) { throw "game files in the release: $($bad.FullName -join ', ')" }

$zip = Join-Path $root "dist\OpenJamboree-v$Version-Windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Output $zip
