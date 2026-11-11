<#
.SYNOPSIS
  闪烁指数测量（静止画面，连采 N 帧，算帧间 |ΔY|）
.DESCRIPTION
  用 GDI 抓合成桌面（SDR 8 位、DWM 重压后的画面），按 Step 像素下采样，
  计算相邻两帧的 |ΔY| 均值 / p95 / 最大，以及"大跳"次数，并打印首末帧平均亮度做 sanity check。
  说明：这是**相对**指标，只用于 A/B（改前 vs 改后），不是绝对亮度。
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\测量-闪烁.ps1 -Seconds 6 -Hz 20 -Tag before
#>
param(
  [int]$Seconds = 6,
  [int]$Hz = 20,
  [int]$Step = 16,
  [string]$Tag = "run",
  [string]$OutDir = ""
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if($OutDir -eq ""){ $OutDir = $PSScriptRoot }

$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$w = $scr.Width; $h = $scr.Height
$bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$nx = [int]($w / $Step) + 1
$ny = [int]($h / $Step) + 1
$cnt = $nx * $ny
$n = [int]($Seconds * $Hz)
if($n -lt 10){ $n = 10 }

Write-Host ("采集中：{0}x{1}, {2} 帧 @ {3} Hz, 下采样 {4}（{5} 采样点/帧）" -f $w,$h,$n,$Hz,$Step,$cnt)
Write-Host "请保持画面静止、不要动鼠标、不要切窗口…"

$prev = $null
$firstLuma = $null
$lastLuma = 0.0
$diffs = New-Object System.Collections.Generic.List[double]
$big = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()
for($f=0; $f -lt $n; $f++){
  $gfx.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size $w,$h))
  $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $bmp.PixelFormat)
  $stride = $data.Stride
  $bytes = New-Object byte[] ($stride * $h)
  [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
  $bmp.UnlockBits($data)

  $cur = New-Object 'double[]' $cnt
  $idx = 0
  $lsum = 0.0
  for($y=0; $y -lt $h; $y+=$Step){
    $row = $y * $stride
    for($x=0; $x -lt $w; $x+=$Step){
      $o = $row + $x*4
      $b = $bytes[$o]; $gg = $bytes[$o+1]; $r = $bytes[$o+2]
      $v = 0.2126*$r + 0.7152*$gg + 0.0722*$b
      $cur[$idx] = $v
      $lsum += $v
      $idx++
    }
  }
  $lmean = $lsum / $idx
  if($null -eq $firstLuma){ $firstLuma = $lmean }
  $lastLuma = $lmean
  if($null -ne $prev){
    $sum = 0.0
    for($i=0; $i -lt $idx; $i++){ $sum += [Math]::Abs($cur[$i] - $prev[$i]) }
    $m = $sum / $idx
    $diffs.Add($m)
    if($m -gt 3.0){ $big++ }
  }
  $prev = $cur
  $target = ($f+1) * (1000.0/$Hz)
  $sleep = $target - $sw.Elapsed.TotalMilliseconds
  if($sleep -gt 0){ Start-Sleep -Milliseconds ([int]$sleep) }
}
$gfx.Dispose(); $bmp.Dispose()

$arr = @($diffs | Sort-Object)
$mean = ($diffs | Measure-Object -Average).Average
$p95 = $arr[[int]([Math]::Floor($arr.Count*0.95))]
$max = ($diffs | Measure-Object -Maximum).Maximum
$csv = Join-Path $OutDir ("flicker_{0}_{1:yyyyMMdd-HHmmss}.csv" -f $Tag,(Get-Date))
"frame,meanAbsDeltaY" | Set-Content $csv -Encoding utf8
for($i=0; $i -lt $diffs.Count; $i++){ ("{0},{1:F3}" -f ($i+1),$diffs[$i]) | Add-Content $csv -Encoding utf8 }

Write-Host ("画面平均亮度（首帧 / 末帧） = {0:F1} / {1:F1}  （都≈0 说明没抓到画面）" -f $firstLuma,$lastLuma)
Write-Host ""
Write-Host ("闪烁指数（mean|ΔY|, 0-255 刻度） = {0:F3}" -f $mean) -ForegroundColor Yellow
Write-Host ("p95 = {0:F3}   max = {1:F3}   大跳(>3) = {2} 次 / {3} 帧" -f $p95,$max,$big,$diffs.Count)
Write-Host ("明细: " + $csv)
Write-Host "用法：改前跑一次（-Tag before），改后跑一次（-Tag after），比 mean 与大跳次数。"
