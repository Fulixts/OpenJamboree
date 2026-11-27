# Two-player online test on one PC: the game in this folder hosts (online=1, online_host empty) and a second copy in
# another folder joins it (online_host=127.0.0.1, its own online_user, ca.der in <exe>_online). Both run in background
# mode through online_probe.ps1 -Continue, the second one -Delay seconds later. Prints the host's matchmaking lines.
#   powershell -File _work/online_pair.ps1 -Second <folder of the second copy> [-After 300] [-Delay 20]
param([Parameter(Mandatory=$true)][string]$Second, [int]$After = 300, [int]$Delay = 20)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$name = "SuperMarioPartyJamboree"
Get-Process $name -ErrorAction SilentlyContinue | Stop-Process -Force
Copy-Item (Join-Path $root "$name.exe"), (Join-Path $root "${name}_code.dll") $Second -Force
$probe = Join-Path $PSScriptRoot "online_probe.ps1"
$hostOut = Join-Path $PSScriptRoot "shots\pair_host.txt"; $guestOut = Join-Path $PSScriptRoot "shots\pair_guest.txt"
New-Item -ItemType Directory -Force (Split-Path $hostOut) | Out-Null
$h = Start-Process powershell -ArgumentList "-NoProfile","-File",$probe,"-After",$After,"-Continue" -RedirectStandardOutput $hostOut -WindowStyle Hidden -PassThru
Start-Sleep -Seconds $Delay
$g = Start-Process powershell -ArgumentList "-NoProfile","-File",$probe,"-Exe",(Join-Path $Second "$name.exe"),"-After",($After - $Delay),"-Continue","-NoKill" -RedirectStandardOutput $guestOut -WindowStyle Hidden -PassThru
$h.WaitForExit(); $g.WaitForExit()
Write-Output "== host"; Get-Content $hostOut | Select-Object -Last 4
Write-Output "== guest"; Get-Content $guestOut | Select-Object -Last 4
Write-Output "== host server"
Select-String -Path (Join-Path $root "$name.log") -Pattern 'online: (matchmaking|room|ticket|grpc .*not impl|.*joined|.*put)|svcBreak|FATAL' |
  Where-Object { $_.Line -notmatch 'ticket request' } | Select-Object -Last 40 | ForEach-Object { $_.Line }
Write-Output "== guest log"
Select-String -Path (Join-Path $Second "$name.log") -Pattern 'online:|svcBreak|FATAL' |
  Where-Object { $_.Line -notmatch 'NAT check|internet request|id token' } | Select-Object -Last 15 | ForEach-Object { $_.Line }
