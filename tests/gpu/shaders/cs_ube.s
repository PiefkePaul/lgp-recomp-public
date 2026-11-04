// Compute shader reading a constant buffer through global memory, as NVN
// compute shaders do: the address of NVN uniform block 8 (bank 11) at
// c0[0x290], storage buffer 0 at c0[0x310]. out[tid] = ubo[tid] + 1.
// Own test shader.
.stage compute
.workgroup 4 1 1
mov $r0 $tidx
mov $r2 c0[0x290] 0xf
mov $r3 c0[0x294] 0xf
lea 0x1 cc $r4 $r0 $r2 0x2
lea hi x 0x1 $r5 $r0 $r3 0x0 0x2
ldg e b32 $r6 ncg[$r4]
iadd32i $r6 $r6 0x1
mov $r8 c0[0x310] 0xf
mov $r9 c0[0x314] 0xf
lea 0x1 cc $r10 $r0 $r8 0x2
lea hi x 0x1 $r11 $r0 $r9 0x0 0x2
stg e b32 ncg[$r10] $r6
exit
