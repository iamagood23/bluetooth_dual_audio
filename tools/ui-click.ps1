# Test driver: sends a mouse click (or wheel) to the DualAudio window at client DIP coordinates.
#   ui-click.ps1 -X 100 -Y 200            click
#   ui-click.ps1 -X 100 -Y 200 -Wheel -3  scroll (negative = down)
#   ui-click.ps1 -Key 0x1B                key press (virtual-key code)
param([double]$X = 0, [double]$Y = 0, [int]$Wheel = 0, [int]$Key = 0, [string]$Class = 'DualAudio.MainWindow')
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UiDrv {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[UiDrv]::SetProcessDPIAware() | Out-Null
$h = [UiDrv]::FindWindow($Class, [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { throw 'window not found' }
$s = [UiDrv]::GetDpiForWindow($h) / 96.0
$px = [int]($X * $s); $py = [int]($Y * $s)
$lp = [IntPtr](($py -shl 16) -bor ($px -band 0xFFFF))
if ($Key -ne 0) {
  [UiDrv]::SendMessage($h, 0x0100, [IntPtr]$Key, [IntPtr]0) | Out-Null   # WM_KEYDOWN
} elseif ($Wheel -ne 0) {
  $pt = New-Object UiDrv+POINT; $pt.X = $px; $pt.Y = $py; [UiDrv]::ClientToScreen($h, [ref]$pt) | Out-Null
  $w = [IntPtr](([int]($Wheel * 120) -shl 16))
  [UiDrv]::SendMessage($h, 0x020A, $w, [IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))) | Out-Null
} else {
  [UiDrv]::SendMessage($h, 0x0200, [IntPtr]0, $lp) | Out-Null   # WM_MOUSEMOVE
  [UiDrv]::SendMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null   # WM_LBUTTONDOWN
  [UiDrv]::SendMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null   # WM_LBUTTONUP
}
