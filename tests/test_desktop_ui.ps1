param(
  [Parameter(Mandatory = $true)][string]$Binary,
  [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DesktopUi {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int length);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)]
  public static extern IntPtr TextMessage(IntPtr window, uint message, IntPtr wp, StringBuilder text);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
  public static string Text(IntPtr window) {
    var text = new StringBuilder(4096);
    TextMessage(window, 0xD, (IntPtr)text.Capacity, text);
    return text.ToString();
  }
}
'@
$fixture = Join-Path $OutputDirectory ("ui-fixture-" + [Diagnostics.Stopwatch]::GetTimestamp())
$work = Join-Path $fixture 'work with spaces'
$config = Join-Path $fixture 'settings with spaces.ini'
$exe = (Resolve-Path -LiteralPath $Binary).Path
$source = Join-Path $fixture 'source archive'
New-Item -ItemType Directory -Path (Join-Path $source 'runtime\src') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Value '# UI fixture only; never configured'
Set-Content -LiteralPath (Join-Path $source 'runtime\src\game_main.cpp') -Value '// UI fixture only; never compiled'
$arguments = "--config `"$config`" --work `"$work`" --source `"$source`" --iso `"$(Join-Path $fixture 'missing image.iso')`""
$p = $null
$handoff = $null
function Start-Ui {
  $script:p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
  $limit = [DateTime]::UtcNow.AddSeconds(12)
  do {
    Start-Sleep -Milliseconds 100
    $script:p.Refresh()
    if ($script:p.HasExited) { throw 'UI exited before showing window.' }
    $script:window = $script:p.MainWindowHandle
  } until ($script:window -ne [IntPtr]::Zero -or [DateTime]::UtcNow -gt $limit)
  if ($script:window -eq [IntPtr]::Zero) { throw 'UI window not ready.' }
}
function Click([int]$id) {
  [void][DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($script:window, $id), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Wait-Status([string]$pattern) {
  $limit = [DateTime]::UtcNow.AddSeconds(10)
  do {
    Start-Sleep -Milliseconds 100
    $text = [DesktopUi]::Text([DesktopUi]::GetDlgItem($script:window, 111))
  } until ($text -match $pattern -or [DateTime]::UtcNow -gt $limit)
  if ($text -notmatch $pattern) { throw "Unexpected UI status: $text" }
  Write-Output $text
}
function Close-Ui {
  [void][DesktopUi]::PostMessage($script:window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
  if (!$script:p.WaitForExit(5000)) { throw 'UI failed to close safely.' }
}
try {
  Start-Ui
  Wait-Status 'Build needed'
  Wait-Status 'includes all stages'
  [void][DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 103), 0x14E, [IntPtr]1, [IntPtr]::Zero)
  $originalIcons = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 114), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($originalIcons.ToInt32() -ne 0) { throw 'Original PSP icons must be the default.' }
  [void][DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 114), 0x14E, [IntPtr]1, [IntPtr]::Zero)
  $scale = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 115), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  $size = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 116), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($scale.ToInt32() -ne 0 -or $size.ToInt32() -ne 1) { throw 'Graphics defaults changed.' }
  $count = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 115), 0x146, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($count.ToInt32() -ne 8) { throw 'All eight render scales must be offered.' }
  $renderer = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 117), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($renderer.ToInt32() -ne 0) { throw 'Software renderer must be the default.' }
  $count = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 117), 0x146, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($count.ToInt32() -ne 4) { throw 'All four renderer choices/status entries must be present.' }
  Click 118
  Wait-Status 'Renderer available: CPU software'
  [void][DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 117), 0x14E, [IntPtr]2, [IntPtr]::Zero)
  [void][DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 115), 0x14E, [IntPtr]7, [IntPtr]::Zero)
  [void][DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 116), 0x14E, [IntPtr]2, [IntPtr]::Zero)
  Click 104
  Wait-Status 'Settings saved'
  if ((Get-Content -LiteralPath $config -Raw) -notmatch 'controls=legacy') { throw 'Profile was not persisted.' }
  if ((Get-Content -LiteralPath $config -Raw) -notmatch 'prompts=modern') { throw 'Icon mode was not persisted.' }
  if ((Get-Content -LiteralPath $config -Raw) -notmatch 'render_scale=8') { throw 'Render scale was not persisted.' }
  if ((Get-Content -LiteralPath $config -Raw) -notmatch 'window_size=1440x816') { throw 'Window size was not persisted.' }
  if ((Get-Content -LiteralPath $config -Raw) -notmatch 'renderer=d3d12') { throw 'Renderer was not persisted.' }
  Click 107
  Wait-Status 'Error:.*standalone Git clone'
  if ([DesktopUi]::IsWindowEnabled([DesktopUi]::GetDlgItem($window, 108))) { throw 'Update should not be enabled after failed check.' }
  Click 106
  Wait-Status 'Error: ISO not found'
  $rect = [DesktopUi+Rect]::new()
  [void][DesktopUi]::GetWindowRect($window, [ref]$rect)
  $bitmap = [Drawing.Bitmap]::new($rect.Right-$rect.Left, $rect.Bottom-$rect.Top)
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  $dc = $graphics.GetHdc()
  try {
    if (![DesktopUi]::PrintWindow($window, $dc, 2)) { throw 'Could not capture owned UI window.' }
  } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
  $bitmap.Save((Join-Path $OutputDirectory 'desktop-ui.png'), [Drawing.Imaging.ImageFormat]::Png)
  $bitmap.Dispose()
  Close-Ui
  Start-Ui
  $profile = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 103), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($profile.ToInt32() -ne 1) { throw 'Remembered profile did not reload.' }
  $icons = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 114), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($icons.ToInt32() -ne 1) { throw 'Remembered icons did not reload.' }
  $scale = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 115), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  $size = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 116), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($scale.ToInt32() -ne 7 -or $size.ToInt32() -ne 2) { throw 'Remembered graphics settings did not reload.' }
  $renderer = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 117), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($renderer.ToInt32() -ne 2) { throw 'Remembered renderer did not reload.' }
  Close-Ui
  $updateTests = Join-Path (Split-Path $exe) 'mgspo-launcher-update-tests.exe'
  if (Test-Path -LiteralPath $updateTests) {
    $tools = Join-Path $work 'updates\ui-fixture'
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    $staged = Join-Path $tools 'mgspo-desktop.exe'
    Copy-Item -LiteralPath $exe -Destination $staged
    & $updateTests --activate-ui-fixture $config $work $source $tools
    if ($LASTEXITCODE -ne 0) { throw 'Could not activate isolated UI fixture.' }
    $arguments = "--config `"$config`" --work `"$work`""
    $handoff = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
    if (!$handoff.WaitForExit(15000) -or $handoff.ExitCode -ne 0) { throw 'Original UI did not hand off to activated UI.' }
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($handoff.Id) AND Name='mgspo-desktop.exe'"
    if (!$child) { throw 'Activated UI child not found.' }
    $p = Get-Process -Id $child.ProcessId
    $window = $p.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { throw 'Activated UI window not ready.' }
    $icons = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 114), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($icons.ToInt32() -ne 1) { throw 'Activated UI lost icon preference.' }
    $scale = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 115), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
    $size = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 116), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($scale.ToInt32() -ne 7 -or $size.ToInt32() -ne 2) { throw 'Activated UI lost graphics settings.' }
    $renderer = [DesktopUi]::SendMessage([DesktopUi]::GetDlgItem($window, 117), 0x147, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($renderer.ToInt32() -ne 2) { throw 'Activated UI lost renderer setting.' }
    Close-Ui
    Remove-Item -LiteralPath $staged -Force
    Start-Ui
    Wait-Status 'activated launcher is unavailable'
    Close-Ui
    Write-Output 'Staged restart, readiness and missing-version fallback checks passed.'
  } else {
    Write-Output 'Staged restart check skipped: build mgspo-launcher-update-tests to enable it.'
  }
  Write-Output 'Native UI settings, error states, archive update refusal, relaunch and close checks passed.'
} finally {
  if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
  if ($handoff -and !$handoff.HasExited) { Stop-Process -Id $handoff.Id -Force }
  Remove-Item -LiteralPath $fixture -Recurse -Force
}
