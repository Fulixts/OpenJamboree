# Walks the game from the title to the worldwide online mode in background mode (off-screen, unfocused, muted) and
# leaves the log for reading: title (L+R) -> players -> character -> plaza -> balloon (skip with +) -> Mario Party ->
# "Todo o mundo" -> internet notice -> community guidelines -> Conectar. Needs a save and online=1 in the .ini.
# The key times count from the moment the title music starts (SM_BGM_TITLE in the log), so slow first boots (shader
# compiles) don't break it. Shots go to _work/shots/online_*.png.
#   powershell -File _work/online_probe.ps1 [-Exe path] [-After 75] [-Trace]
param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuperMarioPartyJamboree.exe"), [int]$After = 150, [switch]$Trace)
$ErrorActionPreference = "Stop"
# seconds after the title music: keys (vk, "+" = together, "@ms" = hold)
# phase 1 counts from the title music, phase 2 from the map music (SM_BGM_MENU_MAP): loads before the map vary a lot
$plan  = @(@(2,"51+45@500"),@(8,"51+45@500"),@(16,"20"),@(22,"20"),@(28,"20"),@(34,"20"),@(39,"0D"))
$plan2 = @(@(3,"20"),@(12,"27"),@(13.5,"27"),@(15,"20"),@(19,"20"),@(23,"25"),@(25,"20"))
$shots = @(14,18,22,27,34)   # after the map music
Add-Type @"
using System; using System.Runtime.InteropServices;
public class PW { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
                  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t); }
"@
function Send-Keys($h, $spec) {
  $p = $spec.Split('@'); $hold = if ($p.Count -gt 1) { [int]$p[1] } else { 200 }
  $vks = @($p[0].Split('+') | ForEach-Object { [Convert]::ToInt32($_, 16) })
  foreach ($v in $vks) { $sc = [PW]::MapVirtualKey($v, 0); [PW]::PostMessage($h, 0x100, [IntPtr]$v, [IntPtr](1 -bor ($sc -shl 16))) | Out-Null }
  Start-Sleep -Milliseconds $hold
  foreach ($v in $vks) { $sc = [PW]::MapVirtualKey($v, 0); [PW]::PostMessage($h, 0x101, [IntPtr]$v, [IntPtr]([int64]0xC0000001 -bor ($sc -shl 16))) | Out-Null } }
$dir = Split-Path -Parent $Exe; $name = [IO.Path]::GetFileNameWithoutExtension($Exe); $log = Join-Path $dir "$name.log"
$out = Join-Path $PSScriptRoot "shots"; New-Item -ItemType Directory -Force $out | Out-Null
Get-Process $name -ErrorAction SilentlyContinue | Stop-Process -Force
$env:SWR_BACKGROUND = "1"; $env:SWR_INPUT_UNFOCUSED = "1"
if ($Trace) { $env:SWR_TRACE_DYN = '8006000-b000000,89e7300-89e7308'; $env:SWR_DUMP_GRPC = '1' }
$p = Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru
Remove-Item Env:SWR_BACKGROUND, Env:SWR_INPUT_UNFOCUSED, Env:SWR_TRACE_DYN, Env:SWR_DUMP_GRPC -ErrorAction SilentlyContinue
$t0 = Get-Date; $title = $null
while (-not $title -and ((Get-Date) - $t0).TotalSeconds -lt 180 -and -not $p.HasExited) {
  Start-Sleep -Milliseconds 500
  if (Test-Path $log) { if (Select-String -Path $log -Pattern 'SM_BGM_TITLE' -Quiet) { $title = Get-Date } } }
if (-not $title) { Write-Output "no title screen after 180 s"; if (-not $p.HasExited) { Stop-Process $p -Force }; exit 1 }
Write-Output "title at $([int]($title - $t0).TotalSeconds) s"
$si = 0; $map = $null; $stage = 0; $last = 0; $done = $false; $phase = 0; $mapA = -100
Add-Type -AssemblyName System.Drawing
function Take-Shot($h, $tag) {
  $cap = Join-Path $dir "captures"; $before = @(Get-ChildItem "$cap\F8_*.png" -ErrorAction SilentlyContinue | ForEach-Object FullName)
  Send-Keys $h "77@100"
  for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 100
    $g = Get-ChildItem "$cap\F8_*.png" -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1
    if ($g) { Start-Sleep -Milliseconds 300; $f = Join-Path $out "$tag.png"; Move-Item $g.FullName $f -Force; return $f } }
  return $null }
# stage 0: title -> L+R until menu00.nro loads; 1: A every 3 s until menu01.nro (balloon tour) loads;
# 2: + (skip) every 3 s until the map music; then the fixed plan2 counted from the map music
function Seen($pat) { Select-String -Path $log -Pattern $pat -Quiet }
while (((Get-Date) - $title).TotalSeconds -lt $After -and -not $p.HasExited) {
  Start-Sleep -Milliseconds 250; $el = ((Get-Date) - $title).TotalSeconds; $p.Refresh(); $h = $p.MainWindowHandle
  if ($h -eq 0) { continue }
  if ($stage -lt 3 -and $el - $last -ge 3) {
    $last = $el
    if ($stage -eq 0) { if (Seen 'menu00.nro') { $stage = 1; Write-Output "plaza menus at $([int]$el) s" } else { Send-Keys $h "51+45@500" } }
    if ($stage -eq 1) { if (Seen 'menu01.nro') { $stage = 2; Write-Output "balloon at $([int]$el) s" } else { Send-Keys $h "58" } }
    if ($stage -eq 2) { if (Seen 'SM_BGM_MENU_MAP') { $stage = 3; $map = Get-Date; Write-Output "map at $([int]$el) s" } else { Send-Keys $h "0D" } } }
  if (-not $map) { continue }
  # stage 3: read the screen from F8 shots (1920x1080) every 2.5 s and press what that screen needs:
  # COM card pink -> Right; "Todo o mundo" card blue -> A; "Nao conectar" pink -> Left; "Conectar" pink -> A, done;
  # anything else (map, notices) -> A
  if (-not $done -and (Seen "internet request accepted")) { $done = $true; Write-Output "the game asked for the internet at $([int]$el) s" }
  # after Conectar: one shot every 15 s, no keys, to see where the game stops
  if ($done) { if ($el - $last -ge 15) { $last = $el; $f = Take-Shot $h ("online_after_" + [int]$el); if ($f) { Write-Output "shot $f" } }; continue }
  if ($el - $last -lt 2.5) { continue }
  $last = $el; $shot = Take-Shot $h ("online_" + $si); $si++
  if (-not $shot) { continue }
  $bmp = [System.Drawing.Bitmap]::FromFile($shot)
  function Px($x, $y) { $c = $bmp.GetPixel($x, $y); @($c.R, $c.G, $c.B) }
  function Pink($c) { $c[0] -gt 220 -and $c[1] -gt 90 -and $c[1] -lt 170 -and $c[2] -gt 110 -and $c[2] -lt 190 }
  function Blue($c) { $c[2] -gt 200 -and $c[0] -lt 140 -and $c[1] -gt 120 }
  # two points per screen, picked from real shots where only that state has that colour (map excluded)
  $com = (Pink (Px 480 380)) -and (Pink (Px 920 620)); $world = (Blue (Px 1040 380)) -and (Blue (Px 1200 560))
  $nocon = (Pink (Px 1100 640)) -and (Pink (Px 1080 680)); $con = (Pink (Px 620 640)) -and (Pink (Px 700 680))
  $bmp.Dispose()
  # phases: 0 = map (A once, then wait up to 10 s for the opponents screen), 1 = opponents screen seen ("Todo o mundo"
  # chosen with A moves on), 2 = online notices and guidelines. an unknown screen gets A only in phase 2, so a late
  # A never picks COM and starts an offline party
  if ($world -or $com) { if ($phase -lt 1) { $phase = 1 } }
  $flip = -not $flip
  $act = if ($world) { "58" } elseif ($com) { if ($flip) { "27@600" } else { "44@600" } } elseif ($nocon) { "25" } elseif ($con) { "58" }
         elseif ($phase -eq 2) { "58" } elseif ($phase -eq 0 -and ($el - $mapA) -ge 10) { $mapA = $el; "58" } else { "" }
  if ($world) { $phase = 2 }
  if (-not $act) { Write-Output ("screen {0,-13} -> wait  ({1:N0} s)" -f "other", $el); continue }
  $what = if ($world) { "world" } elseif ($com) { "COM" } elseif ($nocon) { "nao-conectar" } elseif ($con) { "conectar" } else { "other" }
  Write-Output ("screen {0,-13} -> {1}  ({2:N0} s)" -f $what, $act, $el)
  Start-Sleep -Milliseconds 1000   # an F8 capture stalls a few frames: a key pressed and released inside it is never seen
  if ($act -notmatch "@") { $act += "@600" }
  Send-Keys $h $act
}
if ($p.HasExited) { Write-Output "game exited at $([int]((Get-Date) - $title).TotalSeconds) s after the title" } else { Stop-Process $p -Force; Write-Output "stopped $After s after the title" }
