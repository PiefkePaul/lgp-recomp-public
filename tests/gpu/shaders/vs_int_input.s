// Vertex shader with integer vertex inputs: generic input 1 comes from a
// UINT format (e.g. RGBA8UI bone indices), input 2 from a SINT format; the
// shader reads both as raw integers. Own test shader.
// out0 = (float(in1.x + in1.y), float(in1.z), float(in1.w), float(in2.x)).
.stage vertex
.in 0 xyzw
.in 1 xyzw
.in 2 x
.out 0 xyzw
ld b128 $r0 a[0x80] 0x0
ld b128 $r4 a[0x90] 0x0
ld b32 $r8 a[0xa0] 0x0
iadd $r4 $r4 $r5
i2f f32 s32 $r12 $r4
i2f f32 s32 $r13 $r6
i2f f32 s32 $r14 $r7
i2f f32 s32 $r15 $r8
st b128 a[0x70] $r0 0x0
st b128 a[0x80] $r12 0x0
exit
