// Vertex shader for the instructions added in P4.2 (found in real game shaders):
// XMAD (full 32-bit multiply sequence), IADD3, ISET, I2I (byte select, abs,
// .CC), CSETP, R2P, BFE, PSET, LOP with predicate output, FMUL.RZ/.D2 and
// I2F.RZ, I2F from a selected byte/half (u8 b1, s16 b2). Own test shader; the expected values are derived in render_test.py.
.stage vertex
.in 0 xyzw
.out 0 xyzw
.out 1 xyzw
.out 2 xyzw
ld b128 $r0 a[0x80] 0x0
f2i s32 f32 trunc $r4 $r0
mov32i $r5 0x30000 0xf
iadd $r4 $r4 $r5
mov32i $r7 0x12345 0xf
xmad $r6 $r4 $r7 0x0
xmad mrg $r8 $r4 h1 $r7 0x0
xmad psl cbcc $r6 h1 $r4 h1 $r8 $r6
ld b32 $r10 c0[0x0]
iadd3 $r9 $r6 neg $r4 $r10
iset gt u32 and $r11 $r9 $r4 0x1
iset bf lt and $r12 $r4 $r9 0x1
mov32i $r14 0xfffe0000 0xf
i2i s32 s16 $r13 b2 abs $r14
mov32i $r15 0x0 0xf
i2i u32 u32 cc 0x0 $r15
csetp eq and $p3 0x1 cc 0x1
mov32i $r16 0x3 0xf
r2p pr $r16 0x3
pset bf and and $r19 $p0 $p1 $p3
mov32i $r18 0xabcd1234 0xf
bfe u32 $r17 $r18 0x804
lop and nz $p2 $r20 $r18 0xf0
mov32i $r21 0x0 0xf
$p2 mov32i $r21 0x7 0xf
iadd $r28 $r17 $r20
iadd $r28 $r28 $r21
mov32i $r23 0x3f8ccccd 0xf
mov32i $r24 0x40533333 0xf
fmul ftz rz $r22 $r23 $r24
fmul d2 $r25 $r23 $r24
mov32i $r27 0x1000003 0xf
i2f f32 u32 rz $r26 $r27
lop and 0x1 $r29 $r6 0xffff
shr u32 $r30 $r6 0x10
lop and 0x1 $r31 $r11 0x1
i2f f32 u32 $r29 $r29
i2f f32 u32 $r30 $r30
lop and 0x1 $r9 $r9 0xffff
i2f f32 u32 $r9 $r9
i2f f32 u32 $r31 $r31
i2f f32 s32 $r13 $r13
i2f f32 u32 $r28 $r28
i2f f32 u32 $r21 $r21
i2f f32 u8 $r10 b1 $r18
i2f f32 s16 $r15 b2 $r14
fadd $r21 $r21 $r10
fadd $r21 $r21 $r15
st b128 a[0x70] $r0 0x0
st b32 a[0x80] $r29 0x0
st b32 a[0x84] $r30 0x0
st b32 a[0x88] $r9 0x0
st b32 a[0x8c] $r12 0x0
st b32 a[0x90] $r31 0x0
st b32 a[0x94] $r13 0x0
st b32 a[0x98] $r19 0x0
st b32 a[0x9c] $r28 0x0
st b32 a[0xa0] $r22 0x0
st b32 a[0xa4] $r25 0x0
st b32 a[0xa8] $r26 0x0
st b32 a[0xac] $r21 0x0
exit