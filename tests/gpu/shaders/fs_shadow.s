// Fragment shader: shadow lookup (TEXS 2D.DC) at (0.5, 0.5) with the depth
// reference from c0[0x0]; the comparison result goes to rgb, alpha 1. Own
// test shader.
.stage fragment
.target 0 xyzw
mov32i $r0 0x3f000000 0xf
mov32i $r1 0x3f000000 0xf
mov $r2 c0[0x0] 0xf
texs dc nodep 0x0 $r4 $r0 $r2 0x8 t2d r
mov $r0 $r4 0xf
mov $r1 $r4 0xf
mov $r2 $r4 0xf
mov32i $r3 0x3f800000 0xf
exit
