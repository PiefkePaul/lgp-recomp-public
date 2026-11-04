<#
.SYNOPSIS
  Samples how much of the guest heap of a running Ryujinx process actually holds data.

.DESCRIPTION
  Ryujinx (memory_manager_mode = HostMappedUnsafe) maps the 39-bit guest address space
  linearly into its own host address space. This script finds that host base by looking
  for an NSO image (MOD0 header) at a known guest address, then periodically counts the
  4 KiB pages of the guest heap that contain at least one non-zero byte.

  Ryujinx zero-fills memory on allocation, so "non-zero pages" approximates the memory
  the game has really written. Limits: pages the game only zeroed, and buffers written
  solely by the (host-emulated) GPU, are not counted. Pages Ryujinx protects for GPU
  write tracking cannot be read and are reported separately.

  Only counts leave this script; no memory contents are stored.

.EXAMPLE
  ./ryujinx_heap_scan.ps1 -ProcId 1234 -Csv $env:LGP_PRIVATE_DIR/ryujinx-ref/mem.csv `
      -HeapStart 0x208C600000 -HeapSize 0xC9200000
  HeapStart/HeapSize come from the SetHeapSize lines of the Ryujinx trace log.
#>
param(
    [Parameter(Mandatory)] [int]$ProcId,
    [Parameter(Mandatory)] [string]$Csv,
    [Parameter(Mandatory)] [UInt64]$HeapStart,
    [Parameter(Mandatory)] [UInt64]$HeapSize,
    [UInt64]$ImageAddr = 0x8504000,  # guest address of `main` as printed by Ryujinx' loader
    [int]$EverySec = 20,
    [switch]$Once
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class HeapScan {
  [StructLayout(LayoutKind.Sequential)] public struct MBI { public UInt64 BaseAddress, AllocationBase; public uint AllocationProtect, a; public UInt64 RegionSize; public uint State, Protect, Type, b; }
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern UIntPtr VirtualQueryEx(IntPtr h, UIntPtr addr, out MBI m, UIntPtr len);
  [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, UIntPtr addr, byte[] buf, UIntPtr n, out UIntPtr read);
  const uint MEM_COMMIT = 0x1000;

  static MBI Query(IntPtr h, UInt64 a) { MBI m; VirtualQueryEx(h, (UIntPtr)a, out m, (UIntPtr)Marshal.SizeOf(typeof(MBI))); return m; }

  static bool Read(IntPtr h, UInt64 a, byte[] buf, int n) { UIntPtr got; return ReadProcessMemory(h, (UIntPtr)a, buf, (UIntPtr)n, out got); }

  // Host base B with a valid MOD0 header at B + imageAddr (the first match wins).
  public static UInt64 FindBase(IntPtr h, UInt64 imageAddr) {
    byte[] buf = new byte[8];
    for (UInt64 a = 0; a < 0x7FFFFFFF0000UL; ) {
      MBI m = Query(h, a);
      if (m.RegionSize == 0) break;
      foreach (UInt64 cand in new UInt64[] { m.BaseAddress - imageAddr, m.AllocationBase - imageAddr, m.BaseAddress, m.AllocationBase }) {
        UInt64 at = cand + imageAddr;
        if (!Read(h, at, buf, 8)) continue;
        uint off = BitConverter.ToUInt32(buf, 4);
        if (off == 0 || off > 0x10000000) continue;
        if (Read(h, at + off, buf, 4) && buf[0] == 'M' && buf[1] == 'O' && buf[2] == 'D' && buf[3] == '0') return cand;
      }
      a = m.BaseAddress + m.RegionSize;
    }
    return 0;
  }

  // {committed, readable, nonZero} 4 KiB pages in [start, start + size).
  public static long[] Scan(IntPtr h, UInt64 start, UInt64 size) {
    long committed = 0, readable = 0, nonZero = 0;
    byte[] buf = new byte[1 << 20];
    for (UInt64 a = start, end = start + size; a < end; ) {
      MBI m = Query(h, a);
      if (m.RegionSize == 0) break;
      UInt64 rEnd = Math.Min(end, m.BaseAddress + m.RegionSize);
      if (m.State == MEM_COMMIT) {
        for (UInt64 p = a; p < rEnd; p += (UInt64)buf.Length) {
          int n = (int)Math.Min((UInt64)buf.Length, rEnd - p);
          committed += n / 4096;
          if (!Read(h, p, buf, n)) continue;
          readable += n / 4096;
          for (int pg = 0; pg < n; pg += 4096)
            for (int i = pg; i < pg + 4096; i += 8)
              if (BitConverter.ToUInt64(buf, i) != 0) { nonZero++; break; }
        }
      }
      a = rEnd;
    }
    return new long[] { committed, readable, nonZero };
  }
}
"@

$h = [HeapScan]::OpenProcess(0x0410, $false, $ProcId)  # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
if ($h -eq [IntPtr]::Zero) { throw "cannot open process $ProcId" }
$base = [HeapScan]::FindBase($h, $ImageAddr)
if ($base -eq 0) { throw "guest address space not found (is memory_manager_mode HostMappedUnsafe?)" }
Write-Host ("host base of guest address space: 0x{0:X}" -f $base)

"seconds,committed_pages,readable_pages,nonzero_pages" | Set-Content $Csv
$t0 = Get-Date
while (Get-Process -Id $ProcId -ErrorAction SilentlyContinue) {
    $r = [HeapScan]::Scan($h, $base + $HeapStart, $HeapSize)
    $sec = [int]((Get-Date) - $t0).TotalSeconds
    "$sec,$($r[0]),$($r[1]),$($r[2])" | Add-Content $Csv
    Write-Host ("{0,5}s  non-zero {1:N0} MiB of {2:N0} MiB committed" -f $sec, ($r[2] * 4 / 1024), ($r[0] * 4 / 1024))
    if ($Once) { break }
    Start-Sleep $EverySec
}
