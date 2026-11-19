# Usage: keys.ps1 "RIGHT,RIGHT,ENTER" [delayMs]
# Sends scan-code key presses to the foreground window, only if it is the game (speed.exe).
param([string]$Seq, [int]$Delay = 700)
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class KS {
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KI ki; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, IN[] i, int size);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  public static void Tap(ushort scan, bool ext) {
    var a = new IN[1]; a[0].type = 1; a[0].ki.scan = scan; a[0].ki.flags = 0x0008u | (ext ? 1u : 0u);
    SendInput(1, a, Marshal.SizeOf(typeof(IN))); System.Threading.Thread.Sleep(120);
    a[0].ki.flags |= 0x0002u; SendInput(1, a, Marshal.SizeOf(typeof(IN)));
  }
}
'@
$fg = [KS]::GetForegroundWindow(); $fpid = 0; [void][KS]::GetWindowThreadProcessId($fg, [ref]$fpid)
if ((Get-Process -Id $fpid).ProcessName -ne 'speed') { "game not focused - nothing sent"; exit 1 }
$map = @{ ENTER = @(0x1C, $false); ESC = @(0x01, $false); LEFT = @(0x4B, $true); RIGHT = @(0x4D, $true); UP = @(0x48, $true); DOWN = @(0x50, $true); F5 = @(0x3F, $false); F7 = @(0x41, $false); F8 = @(0x42, $false) }
foreach ($k in $Seq.Split(',')) { $k = $k.Trim().ToUpper(); if ($map.ContainsKey($k)) { [KS]::Tap([uint16]$map[$k][0], [bool]$map[$k][1]); Start-Sleep -Milliseconds $Delay } }
"sent $Seq"
