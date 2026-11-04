param(
  [Parameter(Mandatory = $true)][string]$Binary,
  [Parameter(Mandatory = $true)][string]$Work,
  [Parameter(Mandatory = $true)][string]$Iso,
  [Parameter(Mandatory = $true)][string]$OutputDirectory,
  [string]$SeedSaveDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GameWindow {
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int w, int h, uint flags);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
}
'@
$fixture = Join-Path $OutputDirectory ("window-fixture-" + [Diagnostics.Stopwatch]::GetTimestamp())
New-Item -ItemType Directory -Path $fixture | Out-Null
$p = $null
try {
  $env:MGSPO_GE_THREADS = '2'
  $env:MGSPO_GE_REF = '0'
  if ($SeedSaveDirectory) {
    Copy-Item -LiteralPath (Join-Path $SeedSaveDirectory 'PSP') -Destination $fixture -Recurse
  }
  foreach ($flag in @('--render-scale', '--window-size')) {
    $invalidLog = Join-Path $OutputDirectory ("invalid" + $flag + '.log')
    $p = Start-Process -FilePath $Binary -ArgumentList "$flag 999999999999" -PassThru `
      -RedirectStandardOutput (Join-Path $fixture 'invalid.stdout.log') -RedirectStandardError $invalidLog
    if (!$p.WaitForExit(5000) -or $p.ExitCode -ne 2 -or
        !(Select-String -Path $invalidLog -Pattern 'must be' -Quiet)) { throw "Invalid $flag did not fail explicitly." }
  }
  $frame = Join-Path $OutputDirectory 'game-window-frame.png'
  $press = ((0..49 | ForEach-Object { "${_}:0x2008:0.4" }) +
            (55..94 | ForEach-Object { "${_}:0x2000:0.4" })) -join ','
  $arguments = "`"$(Join-Path $Work 'boot\boot.elf')`" --disc `"$(Join-Path $Work 'disc')`" --iso `"$Iso`" --save `"$fixture`" --timeout 110 --controls modern --render-scale 2 --window-size 480x272 --press `"$press`" --dump-frame `"$frame`""
  $p = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru `
    -RedirectStandardOutput (Join-Path $OutputDirectory 'game-window.stdout.log') `
    -RedirectStandardError (Join-Path $OutputDirectory 'game-window.stderr.log')
  $limit = [DateTime]::UtcNow.AddSeconds(12)
  do {
    Start-Sleep -Milliseconds 100
    $p.Refresh()
    if ($p.HasExited) { throw 'Game exited before showing window.' }
    $window = $p.MainWindowHandle
  } until ($window -ne [IntPtr]::Zero -or [DateTime]::UtcNow -gt $limit)
  if ($window -eq [IntPtr]::Zero) { throw 'Game window not ready.' }
  $client = [GameWindow+Rect]::new()
  $outer = [GameWindow+Rect]::new()
  [void][GameWindow]::GetClientRect($window, [ref]$client)
  [void][GameWindow]::GetWindowRect($window, [ref]$outer)
  if ($client.Right -ne 480 -or $client.Bottom -ne 272) { throw 'Initial client size did not match selected preset.' }
  $borderW = $outer.Right - $outer.Left - $client.Right
  $borderH = $outer.Bottom - $outer.Top - $client.Bottom
  if (![GameWindow]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, 1000+$borderW, 700+$borderH, 0x16)) {
    throw 'Could not resize owned game window.'
  }
  Start-Sleep -Seconds 100
  [void][GameWindow]::GetClientRect($window, [ref]$client)
  if ($client.Right -ne 1000 -or $client.Bottom -ne 700) { throw 'Manual resize was not retained.' }
  $bitmap = [Drawing.Bitmap]::new(1000, 700)
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  $dc = $graphics.GetHdc()
  try {
    if (![GameWindow]::PrintWindow($window, $dc, 1)) { throw 'Could not capture game client.' }
  } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
  try {
    if (($bitmap.GetPixel(500, 8).ToArgb() -band 0xFFFFFF) -ne 0 -or
        ($bitmap.GetPixel(500, 691).ToArgb() -band 0xFFFFFF) -ne 0) { throw 'Expected black letterboxing after resize.' }
    $coloured = 0
    for ($y=100; $y -lt 600; $y+=20) {
      for ($x=20; $x -lt 1000; $x+=20) {
        if (($bitmap.GetPixel($x, $y).ToArgb() -band 0xFFFFFF) -ne 0) { $coloured++ }
      }
    }
    if ($coloured -lt 20) { throw 'Client capture contains no verifiable game image.' }
    $bitmap.Save((Join-Path $OutputDirectory 'game-window.png'), [Drawing.Imaging.ImageFormat]::Png)
  } finally { $bitmap.Dispose() }
  if (!$p.WaitForExit(25000) -or $p.ExitCode -ne 0) { throw 'Game did not exit on its bounded timeout.' }
  $image = [Drawing.Image]::FromFile($frame)
  try {
    if ($image.Width -ne 960 -or $image.Height -ne 544) { throw 'Expected real 2x surface independent of window client size.' }
  } finally { $image.Dispose() }
  Write-Output '2x display, 480x272 initial window, manual 1000x700 resize and black letterboxing passed.'
} finally {
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item -LiteralPath $fixture -Recurse -Force
}
