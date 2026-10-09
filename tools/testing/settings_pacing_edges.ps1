# Frame pacing edge cases for the settings window, on a test copy (see build_test_instance.cmd):
# scrolling, minimizing while scrolling, restoring, resizing, sitting behind another window, then
# scrolling again. Prints phase markers between the copy's once-a-second "settings:" and "loop:"
# lines: a stall shows as missing or very low frames/s while visible, a spin as thousands of wakes.
#
#   powershell -ExecutionPolicy Bypass -File tools\testing\settings_pacing_edges.ps1 [-Tag name]
#
# It moves the mouse for about 35 s. Clicks land only on the test copy's window. Output stays in
# build\uitest\<Tag>\.
param([string]$Tag = 'edges')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$dir = Join-Path $root "build\uitest\$Tag"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory $dir | Out-Null
Copy-Item "$root\build\Release-user\*" $dir -Recurse
"[General]`r`nconfigVersion=2`r`nlanguage=en-US`r`nsettingsPage=1`r`nfirstRunDone=1`r`nautoUpdate=0`r`n`r`n[Hud]`r`nhideWhenIdle=1`r`n" |
    Out-File (Join-Path $dir 'config.ini') -Encoding ascii

Add-Type @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class FpsoEdge {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int data, IntPtr extra);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  // The settings window of process pid (its overlay and hidden message window are other classes).
  public static IntPtr Settings(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr lp) {
      uint p; GetWindowThreadProcessId(h, out p);
      var c = new System.Text.StringBuilder(64); GetClassNameW(h, c, 64);
      if (p == pid && c.ToString() == "FPSOverlayTest.Settings") { found = h; return false; }
      return true; }, IntPtr.Zero);
    return found;
  }
  public static bool Front(IntPtr h) { keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero); SetForegroundWindow(h); Thread.Sleep(150); return GetForegroundWindow() == h; }
  public static bool OnWindow(IntPtr h, int x, int y) { POINT p; p.x = x; p.y = y; return GetAncestor(WindowFromPoint(p), 2) == h; }
  public static void Scroll(int notches, int gapMs) {
    for (int i = 0; i < notches; i++) { mouse_event(0x0800, 0, 0, -120, IntPtr.Zero); Thread.Sleep(gapMs); }
    for (int i = 0; i < notches; i++) { mouse_event(0x0800, 0, 0, 120, IntPtr.Zero); Thread.Sleep(gapMs); }
  }
}
'@
[void][FpsoEdge]::SetProcessDpiAwarenessContext([IntPtr](-4))
$marks = @()
function Mark($m) { $script:marks += ("{0} ---- {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m) }

$saved = New-Object FpsoEdge+POINT; [void][FpsoEdge]::GetCursorPos([ref]$saved)
$proc = Start-Process (Join-Path $dir 'FPSOverlay.exe') -PassThru
try {
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $h = [FpsoEdge]::Settings([uint32]$proc.Id) }
    if ($h -eq [IntPtr]::Zero) { throw 'The settings window did not appear.' }
    Start-Sleep -Milliseconds 800
    # Brings the window to the front and parks the cursor on its page; scrolls only reach it then.
    function Focus {
        $front = [FpsoEdge]::Front($h)
        $r = New-Object FpsoEdge+RECT; [void][FpsoEdge]::GetWindowRect($h, [ref]$r)
        $x = [int](($r.l + $r.r) / 2 + 100); $y = [int](($r.t + $r.b) / 2)
        [void][FpsoEdge]::SetCursorPos($x, $y); Start-Sleep -Milliseconds 300
        $ok = $front -and [FpsoEdge]::OnWindow($h, $x, $y)
        Mark ("in front: {0}" -f $ok)
        if (-not $ok) { throw 'The test window is not on top; stopping.' }
    }

    Focus; Start-Sleep -Seconds 2
    Mark 'scroll'; [FpsoEdge]::Scroll(5, 80); Start-Sleep -Seconds 2
    Mark 'minimize while scrolling'; [FpsoEdge]::Scroll(2, 40); [void][FpsoEdge]::ShowWindow($h, 6); Start-Sleep -Seconds 3
    Mark 'restore'; [void][FpsoEdge]::ShowWindow($h, 9); Focus; Start-Sleep -Seconds 2
    Mark 'scroll after restore'; [FpsoEdge]::Scroll(5, 80); Start-Sleep -Seconds 3
    Mark 'idle in front (expect 30/s)'; Start-Sleep -Seconds 2
    Mark 'resize while scrolling'
    $r = New-Object FpsoEdge+RECT; [void][FpsoEdge]::GetWindowRect($h, [ref]$r)
    [FpsoEdge]::Scroll(2, 40)
    [void][FpsoEdge]::SetWindowPos($h, [IntPtr]::Zero, $r.l, $r.t, ($r.r - $r.l) + 120, ($r.b - $r.t) + 60, 0x0014)
    Start-Sleep -Milliseconds 300; Focus
    Mark 'scroll after resize'; [FpsoEdge]::Scroll(5, 80); Start-Sleep -Seconds 2
    Mark 'minimized, then shown without focus'; [void][FpsoEdge]::ShowWindow($h, 7); Start-Sleep -Seconds 3
    [void][FpsoEdge]::ShowWindow($h, 4); Start-Sleep -Seconds 3
    Mark 'focus and scroll'; [void][FpsoEdge]::ShowWindow($h, 9); Focus; [FpsoEdge]::Scroll(5, 80); Start-Sleep -Seconds 2
    Mark 'end'
} finally {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    [void]$proc.WaitForExit(5000)     # it holds frames.csv and its log open until it is gone
    [void][FpsoEdge]::SetCursorPos($saved.x, $saved.y)
}
$lines = Get-Content (Join-Path $dir 'FPSOverlay.log') | Where-Object { $_ -match 'settings: |loop: ' } |
    ForEach-Object { (($_ -replace '^\d{4}-\d\d-\d\d ', '') -replace ' \[info\]', '') -replace 'build .* per frame, ', '' }
($lines + $marks) | Sort-Object

