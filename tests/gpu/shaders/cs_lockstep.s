// Compute shader with barriers in divergent code: threads 24..31 exit early,
// the others run a loop with two barriers per iteration (each thread adds to
// its shared word, then reads the neighbour's). Also local memory, sub-word
// global stores/loads and atomics that need compare-exchange loops in WGSL.
// Own test shader. c0[0x310] output (16 bytes per thread), c0[0x320] bytes,
// c0[0x330] counters: guest address low, high.
.stage compute
.workgroup 32 1 1
.shared 0x80
mov $r0 $tidx
shl $r1 $r0 0x2
isetp ge u32 and $p0 0x1 $r0 0x18 0x1
$p0 exit
mov32i $r2 0x0 0xf
mov32i $r10 0x0 0xf
iadd $r4 $r0 0x1
isetp eq u32 and $p2 0x1 $r4 0x18 0x1
sel $r4 0x0 $r4 $p2
shl $r5 $r4 0x2
iadd $r6 $r0 0x1
st b32 s[$r1] 0x0
isetp eq u32 and $p3 0x1 $r0 0x0 0x1
mov32i $r8 0x80000000 0xf
$p3 st b32 s[0x60] $r8
loop:
ld b32 $r3 s[$r1]
iadd $r3 $r3 $r6
st b32 s[$r1] $r3
bar sync 0x0 0x0 0x1
ld b32 $r7 s[$r5]
iadd $r10 $r10 $r7
bar sync 0x0 0x0 0x1
iadd32i $r2 $r2 0x1
isetp lt u32 and $p1 0x1 $r2 0x3 0x1
$p1 bra @loop
st b32 l[0x10] $r10
st b32 l[0x14] $r0
ld b64 $r12 l[0x10]
mov $r20 c0[0x310] 0xf
mov $r21 c0[0x314] 0xf
lea 0x1 cc $r22 $r0 $r20 0x4
lea hi x 0x1 $r23 $r0 $r21 0x0 0x4
stg e b64 ncg[$r22] $r12
mov $r24 c0[0x320] 0xf
mov $r25 c0[0x324] 0xf
iadd cc $r24 $r24 $r0
iadd x $r25 $r25 0x0
iscadd $r26 $r0 $r0 0x2
iadd $r26 $r26 0x70
stg e u8 ncg[$r24] $r26
ldg e s8 $r27 ncg[$r24]
stg e b32 ncg[$r22+0x8] $r27
mov $r14 c0[0x330] 0xf
mov $r15 c0[0x334] 0xf
mov32i $r28 0x6 0xf
atom inc e u32 $r29 g[$r14] $r28
iadd $r30 $r0 -0xa
atom min e s32 $r29 g[$r14+0x4] $r30
atoms max s32 $r31 s[0x60] $r30
bar sync 0x0 0x0 0x1
ld b32 $r31 s[0x60]
stg e b32 ncg[$r22+0xc] $r31
exit
