// Vertex shader: clip position = M * position (M column-major in c3[0x00..0x3c], NVN uniform block 0),
// passes colour and texture coordinates through. Own test shader.
.stage vertex
.in 0 xyzw
.in 1 xyzw
.in 2 xy
.out 0 xyzw
.out 1 xy
ld b128 $r0 a[0x80] 0x0
ld b128 $r4 a[0x90] 0x0
ld b64 $r8 a[0xa0] 0x0
fmul $r12 $r0 c3[0x0]
ffma $r12 $r1 c3[0x10] $r12
ffma $r12 $r2 c3[0x20] $r12
ffma $r12 $r3 c3[0x30] $r12
fmul $r13 $r0 c3[0x4]
ffma $r13 $r1 c3[0x14] $r13
ffma $r13 $r2 c3[0x24] $r13
ffma $r13 $r3 c3[0x34] $r13
fmul $r14 $r0 c3[0x8]
ffma $r14 $r1 c3[0x18] $r14
ffma $r14 $r2 c3[0x28] $r14
ffma $r14 $r3 c3[0x38] $r14
fmul $r15 $r0 c3[0xc]
ffma $r15 $r1 c3[0x1c] $r15
ffma $r15 $r2 c3[0x2c] $r15
ffma $r15 $r3 c3[0x3c] $r15
st b128 a[0x70] $r12 0x0
st b128 a[0x80] $r4 0x0
st b64 a[0x90] $r8 0x0
exit
