// Vertex shader exercising integer, conversion and special-function ops,
// a dynamically indexed constant load and the vertex id. Own test shader.
.stage vertex
.in 0 xyzw
.out 0 xyzw
.out 1 xyzw
ld b128 $r0 a[0x80] 0x0
ld b32 $r16 a[0x2fc] 0x0
f2i s32 f32 trunc $r4 $r0
iadd $r5 $r4 c0[0x0]
iadd $r5 $r5 $r16
shl $r6 $r5 0x2
shr $r7 $r6 0x1
shr u32 $r7 $r7 $r5
lop and 0x1 $r8 $r7 0xff
lop32i xor $r8 $r8 0x55
iscadd $r9 $r8 $r5 0x3
imnmx $r9 $r9 $r4 0x1
isetp lt and $p0 0x1 $r9 0x0 0x1
ld b32 $r10 c1[$r9+0x10]
sel $r11 $r10 $r9 not $p0
i2f f32 s32 $r12 $r11
mufu rsq $r13 $r12
fmnmx $r13 $r13 0x3f800000 0x1
f2f f32 f32 round $r14 $r13
fset bf lt and $r15 $r14 $r13 0x1
psetp and and $p1 0x1 $p0 not $p0 0x1
$p1 mov32i $r15 0x40000000 0xf
mufu sin $r12 $r12
mufu ex2 $r13 $r13
i2f f32 s32 $r4 $r4
i2f f32 s32 $r5 $r5
i2f f32 s32 $r6 $r6
i2f f32 s32 $r7 $r7
st b128 a[0x70] $r0 0x0
st b128 a[0x80] $r12 0x0
st b128 a[0x90] $r4 0x0
exit
