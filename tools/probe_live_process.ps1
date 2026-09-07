# Read-only inspection of a live SinEpisodes.exe.
#
# This is the technique that proved every vtable index and struct offset in
# README.md -- against a running game, with ReadProcessMemory, without shipping a
# build and waiting for a log. Nothing here writes to the target process.
#
# Usage:
#   . .\tools\probe_live_process.ps1        # dot-source, then use the helpers
#   Get-Modules | Where-Object Name -match 'client|engine'
#   Hex-Dump 0x240FDAD0 48
#   Read-U32 0x24377060
#
# Worked example -- finding IViewRender, as done on 2026-08-03:
#   $vt   = Read-U32 (Read-U32 0x24377060)     # `view` global -> object -> vtable
#   Hex-Dump (Read-U32 ($vt + 11*4)) 16        # slot 11 GetViewSetup
#   # -> 8D 41 0C C3   =   lea eax,[ecx+0x0C] ; ret
#
# NOTE 32-bit modules are under-reported by the usual Process.Modules and by
# `tasklist /m`, which is why this uses Toolhelp32 with TH32CS_SNAPMODULE32.

# Read-only probe of a live SinEpisodes.exe.
# 32-bit target, so modules must come from Toolhelp32 with TH32CS_SNAPMODULE32 --
# the usual Process.Modules under-reports for WOW64 targets.

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Mem {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out IntPtr read);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
  public static extern bool Module32First(IntPtr snap, ref MODULEENTRY32 me);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
  public static extern bool Module32Next(IntPtr snap, ref MODULEENTRY32 me);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);

  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
  public struct MODULEENTRY32 {
    public uint dwSize; public uint th32ModuleID; public uint th32ProcessID;
    public uint GlblcntUsage; public uint ProccntUsage; public IntPtr modBaseAddr;
    public uint modBaseSize; public IntPtr hModule;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string szModule;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string szExePath;
  }
}
"@

# Both names: sinvr_launcher.exe runs the game from a LARGE_ADDRESS_AWARE copy
# called SinEpisodes_laa.exe, so matching only "SinEpisodes" finds nothing on a
# normal modded session.
$proc = Get-Process -Name 'SinEpisodes_laa','SinEpisodes' -ErrorAction SilentlyContinue |
        Select-Object -First 1
if (-not $proc) {
    throw "Neither SinEpisodes_laa.exe nor SinEpisodes.exe is running. Start the game (it does not need the VR mod for read-only inspection) and dot-source this again."
}
Write-Host "probe: attached to $($proc.ProcessName) (pid $($proc.Id))"
$pid_ = $proc.Id

$h = [Mem]::OpenProcess(0x0410, $false, $pid_)   # QUERY_INFORMATION | VM_READ
if ($h -eq [IntPtr]::Zero) {
    $err = [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message
    throw "OpenProcess failed on pid $pid_`: $err`nIf this says access denied, run PowerShell as administrator."
}

$script:handle = $h

function Get-Modules {
  $TH32CS_SNAPMODULE   = 0x00000008
  $TH32CS_SNAPMODULE32 = 0x00000010
  $snap = [Mem]::CreateToolhelp32Snapshot($TH32CS_SNAPMODULE -bor $TH32CS_SNAPMODULE32, $pid_)
  $me = New-Object Mem+MODULEENTRY32
  $me.dwSize = [Runtime.InteropServices.Marshal]::SizeOf($me)
  $out = @()
  if ([Mem]::Module32First($snap, [ref]$me)) {
    do {
      $out += [pscustomobject]@{ Name=$me.szModule; Base=[int64]$me.modBaseAddr; Size=$me.modBaseSize }
      $me.dwSize = [Runtime.InteropServices.Marshal]::SizeOf($me)
    } while ([Mem]::Module32Next($snap, [ref]$me))
  }
  [Mem]::CloseHandle($snap) | Out-Null
  $out
}

function Read-Bytes([int64]$addr, [int]$len) {
  $buf = New-Object byte[] $len
  $read = [IntPtr]::Zero
  if (-not [Mem]::ReadProcessMemory($script:handle, [IntPtr]$addr, $buf, $len, [ref]$read)) { return $null }
  $buf
}

function Read-U32([int64]$addr) {
  $b = Read-Bytes $addr 4
  if ($null -eq $b) { return $null }
  [BitConverter]::ToUInt32($b, 0)
}

function Read-F32([int64]$addr) {
  $b = Read-Bytes $addr 4
  if ($null -eq $b) { return $null }
  [BitConverter]::ToSingle($b, 0)
}

function Hex-Dump([int64]$addr, [int]$len) {
  $b = Read-Bytes $addr $len
  if ($null -eq $b) { Write-Output "  <unreadable at 0x$($addr.ToString('X8'))>"; return }
  for ($i = 0; $i -lt $len; $i += 16) {
    $n = [Math]::Min(16, $len - $i)
    $hex = ($b[$i..($i+$n-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
    Write-Output ("  {0:X8}  {1}" -f ($addr + $i), $hex)
  }
}
