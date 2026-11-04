// Compute shader: global loads/stores with 64-bit addresses from the driver
// constant buffer (LEA/LEA.HI.X and IADD.CC/IADD.X chains), shared memory with
// a barrier (reverse within the workgroup), shared and global atomics,
// LDG/STG.64/.128, SHF funnel shifts, MEMBAR before BAR. Shared memory starts
// undefined, as on the hardware: thread 0 clears the counter first. Own test shader.
// c0[0x310] input, c0[0x320] output (8 bytes per thread), c0[0x330] counters,
// c0[0x340] output 2 (16 bytes per thread): guest address low, high.
.stage compute
.workgroup 64 1 1
.shared 0x110
mov $r0 $tidx
mov $r1 $ctaidx
iscadd $r2 $r1 $r0 0x6
lea 0x1 cc $r4 $r2 c0[0x310] 0x2
lea hi x 0x1 $r5 $r2 c0[0x314] 0x0 0x2
ldg e b32 $r6 ncg[$r4]
shl $r7 $r0 0x2
isetp eq u32 and $p0 0x1 $r0 0x0 0x1
$p0 st b32 s[0x100] 0x0
membar cta
bar sync 0x0 0x0 0x1
st b32 s[$r7] $r6
mov32i $r16 0x1 0xf
atoms add u32 $r17 s[0x100] $r16
bar sync 0x0 0x0 0x1
iadd $r8 neg $r7 0xfc
ld b32 $r9 s[$r8]
ld b32 $r18 s[0x100]
shl $r12 $r2 0x3
iadd cc $r10 $r12 c0[0x320]
iadd x $r11 0x0 c0[0x324]
stg e b32 ncg[$r10] $r9
stg e b32 ncg[$r10+0x4] $r18
mov $r14 c0[0x330] 0xf
mov $r15 c0[0x334] 0xf
atom add e u32 $r13 g[$r14] $r6
red add e u32 g[$r14+0x4] $r16
atom max e s32 $r13 g[$r14+0x8] $r6
atom min e s32 $r13 g[$r14+0xc] $r6
atom min e u32 $r13 g[$r14+0x10] $r6
shr u32 $r19 $r2 0x1
lea 0x1 cc $r4 $r19 c0[0x310] 0x3
lea hi x 0x1 $r5 $r19 c0[0x314] 0x0 0x3
ldg e b64 $r20 ncg[$r4]
shf r u64 $r24 $r20 0x4 $r21
shf r s64 hi $r25 $r20 0x4 $r21
mov32i $r23 0x24 0xf
shf l w $r26 $r20 $r23 $r21
lea 0x1 $r27 $r0 $r2 0x3
shl $r12 $r2 0x4
iadd cc $r10 $r12 c0[0x340]
iadd x $r11 0x0 c0[0x344]
stg e b128 ncg[$r10] $r24
exit
