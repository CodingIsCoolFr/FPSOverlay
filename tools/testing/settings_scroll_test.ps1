# Measures the settings window while it scrolls: starts a fresh test copy (see
# build_test_instance.cmd), opens its settings, idles, then scrolls the Appearance and Overlay pages
# with real mouse-wheel input. Prints the main thread's CPU per phase and the copy's own
# once-a-second "settings:" lines (frames/s, build and Present time per frame).
#
#   powershell -ExecutionPolicy Bypass -File tools\testing\settings_scroll_test.ps1 [-Tag name] [-Build folder]
#
# It moves the mouse for about 40 s. Clicks land only on the test copy's window. Output and the
# copy's log stay in build\uitest\<Tag>\. Run it from an administrator prompt to test the copy
# with administrator rights, as the app normally runs (frame capture, GPU priority).
param(
    [string]$Tag = 'scroll',
    [string]$Build = 'build\Release-user',     # the test copy to run, relative to the repository
    [int]$IdleSeconds = 6
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$dir = Join-Path $root "build\uitest\$Tag"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory $dir | Out-Null
Copy-Item (Join-Path $root "$Build\*") $dir -Recurse
"[General]`r`nconfigVersion=2`r`nlanguage=en-US`r`nsettingsPage=1`r`nfirstRunDone=1`r`nautoUpdate=0`r`n`r`n[Hud]`r`nhideWhenIdle=1`r`n" |
    Out-File (Join-Path $dir 'config.ini') -Encoding ascii

Add-Type @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class FpsoUi {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int data, IntPtr extra);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
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
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  // An Alt tap first: Windows lets the process that sent the last input take the foreground.
  public static bool Front(IntPtr h) { keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero); SetForegroundWindow(h); Thread.Sleep(150); return GetForegroundWindow() == h; }
  // Clicks only when the point is on window h, never on another app.
  public static bool Click(IntPtr h, int x, int y) {
    POINT p; p.x = x; p.y = y;
    if (GetAncestor(WindowFromPoint(p), 2) != h) return false;
    SetCursorPos(x, y); Thread.Sleep(60); mouse_event(2, 0, 0, 0, IntPtr.Zero); Thread.Sleep(40); mouse_event(4, 0, 0, 0, IntPtr.Zero);
    return true;
  }
  public static void Wheel(int delta) { mouse_event(0x0800, 0, 0, delta, IntPtr.Zero); }
  // Quick flicks of six notches, then single notches 250 ms apart; about 13 s.
  public static void ScrollPattern() {
    for (int r = 0; r < 4; r++) {
      for (int i = 0; i < 6; i++) { Wheel(-120); Thread.Sleep(70); }
      Thread.Sleep(700);
      for (int i = 0; i < 6; i++) { Wheel(120); Thread.Sleep(70); }
      Thread.Sleep(700);
    }
    for (int i = 0; i < 8; i++) { Wheel(-120); Thread.Sleep(250); }
    for (int i = 0; i < 8; i++) { Wheel(120); Thread.Sleep(250); }
  }
}
'@
[void][FpsoUi]::SetProcessDpiAwarenessContext([IntPtr](-4))

$saved = New-Object FpsoUi+POINT; [void][FpsoUi]::GetCursorPos([ref]$saved)
$proc = Start-Process (Join-Path $dir 'FPSOverlay.exe') -PassThru
try {
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $h = [FpsoUi]::Settings([uint32]$proc.Id) }
    if ($h -eq [IntPtr]::Zero) { throw 'The settings window did not appear.' }
    Start-Sleep -Milliseconds 800
    $q = 0; $mainTid = [FpsoUi]::GetWindowThreadProcessId($h, [ref]$q)
    $scale = [FpsoUi]::GetDpiForWindow($h) / 96.0
    $cr = New-Object FpsoUi+RECT; [void][FpsoUi]::GetClientRect($h, [ref]$cr)
    $o = New-Object FpsoUi+POINT; [void][FpsoUi]::ClientToScreen($h, [ref]$o)
    function C($x, $y) { @([int]($o.x + $x * $scale), [int]($o.y + $y * $scale)) }
    $cw = $cr.r / $scale; $ch = $cr.b / $scale
    # Page area right of the 13.5 em sidebar, between the 3.4 em header and 3.6 em footer (16 px em).
    $page = C (216 + ($cw - 216) / 2) (54 + ($ch - 54 - 58) / 2)
    $tabOverlay = C 60 89
    $tabAppearance = C 60 131

    function MainCpuMs { $proc.Refresh(); ($proc.Threads | Where-Object Id -eq $mainTid).TotalProcessorTime.TotalMilliseconds }
    $results = @()
    function Phase($name, [scriptblock]$body) {
        $c0 = MainCpuMs; $t0 = Get-Date
        & $body
        $secs = ((Get-Date) - $t0).TotalSeconds
        $script:results += [pscustomobject]@{ Phase = $name; Start = $t0.ToString('HH:mm:ss.f'); Seconds = [math]::Round($secs, 1)
                                              MainThreadCpuPct = [math]::Round(((MainCpuMs) - $c0) / 10 / $secs, 1) }
    }
    function Tab($pt) { if (-not [FpsoUi]::Click($h, $pt[0], $pt[1])) { throw 'The test window is not on top; stopping.' }; [void][FpsoUi]::SetCursorPos($page[0], $page[1]) }

    if (-not [FpsoUi]::Front($h)) { throw 'Could not bring the test window to the front.' }
    Tab $tabAppearance
    Start-Sleep -Milliseconds 1500
    Phase 'idle (in front)' { Start-Sleep -Seconds $IdleSeconds }
    Phase 'scroll Appearance' { [FpsoUi]::ScrollPattern() }
    Phase 'after scroll' { Start-Sleep -Seconds 3 }
    Tab $tabOverlay
    Phase 'idle on Overlay' { Start-Sleep -Seconds $IdleSeconds }
    Phase 'scroll Overlay' { [FpsoUi]::ScrollPattern() }
    Phase 'after scroll 2' { Start-Sleep -Seconds 2 }
} finally {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    [void]$proc.WaitForExit(5000)     # it holds frames.csv and its log open until it is gone
    [void][FpsoUi]::SetCursorPos($saved.x, $saved.y)
}
$results | Format-Table -AutoSize | Out-String

# Per-frame summary from frames.csv (fast frames only): how many frames came one refresh after
# the last, how many a refresh late (shown twice), and how evenly the page moved while it glided.
$rows = Import-Csv (Join-Path $dir 'frames.csv')
$gaps = @($(for ($i = 1; $i -lt $rows.Count; $i++) { [double]$rows[$i].ms - [double]$rows[$i - 1].ms }) | Where-Object { $_ -gt 1 -and $_ -lt 40 } | Sort-Object)
$refresh = $gaps[[int]($gaps.Count / 2)]     # the median gap is one refresh
$onTime = 0; $late = 0; $other = 0; $steps = @{}
for ($i = 1; $i -lt $rows.Count; $i++) {
    $gap = [double]$rows[$i].ms - [double]$rows[$i - 1].ms
    if ($gap -gt 40) { continue }                         # a new burst of scrolling
    $n = [math]::Round($gap / $refresh)
    if ($n -eq 1) { $onTime++ } elseif ($n -ge 2) { $late++ } else { $other++ }
    $step = [math]::Abs([int]$rows[$i].scroll - [int]$rows[$i - 1].scroll)
    if ($step -gt 0) { $steps[$step] = 1 + [int]$steps[$step] }
}
"Frames ({0:F0} Hz display): {1} one refresh apart, {2} late (shown twice), {3} early" -f (1000 / $refresh), $onTime, $late, $other
"Scroll steps while gliding (pixels per frame: frames): " + (($steps.Keys | Sort-Object | ForEach-Object { "${_}: $($steps[$_])" }) -join ', ')
Get-Content (Join-Path $dir 'FPSOverlay.log') | Where-Object { $_ -match 'settings: |loop: ' } |
    ForEach-Object { ($_ -replace '^\d{4}-\d\d-\d\d ', '') -replace ' \[info\]', '' }

