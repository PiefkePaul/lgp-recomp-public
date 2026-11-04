// Fragment shader: percentage-closer filtering in the shader, as LGP's
// shadow shaders do it: 10 shadow lookups (TEXS 2D.DC) at (u, v) + offset k,
// u/v in c0[0x4]/c0[0x8], offsets in c0[0x10 + 8k], the depth reference in
// c0[0x0]; rgb = mean of the 10 results, alpha 1. Own test shader.
.stage fragment
.target 0 xyzw
mov $r2 c0[0x0] 0xf
mov $r9 c0[0x4] 0xf
mov $r10 c0[0x8] 0xf
mov32i $r8 0x0 0xf
fadd $r0 $r9 c0[0x10]
fadd $r1 $r10 c0[0x14]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x18]
fadd $r1 $r10 c0[0x1c]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x20]
fadd $r1 $r10 c0[0x24]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x28]
fadd $r1 $r10 c0[0x2c]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x30]
fadd $r1 $r10 c0[0x34]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x38]
fadd $r1 $r10 c0[0x3c]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x40]
fadd $r1 $r10 c0[0x44]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x48]
fadd $r1 $r10 c0[0x4c]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x50]
fadd $r1 $r10 c0[0x54]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fadd $r0 $r9 c0[0x58]
fadd $r1 $r10 c0[0x5c]
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
fadd $r8 $r8 $r4
fmul32i $r0 $r8 0x3dcccccd
mov $r1 $r0 0xf
mov $r2 $r0 0xf
mov32i $r3 0x3f800000 0xf
exit
