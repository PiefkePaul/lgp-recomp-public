// Fragment shader: texel offsets (TEX.AOFFI with a constant offset and with
// one only known at run time), LOD bias (TEX.LB) and implicit LOD. Own test
// shader. c0[0x0] = packed offsets for the run-time case.
.stage fragment
.in 1 perspective xy
.target 0 xyzw
mov32i $r0 0x3ec00000 0xf
mov32i $r1 0x3ec00000 0xf
mov32i $r2 0xf1 0xf
tex lz aoffi $r4 $r0 $r2 0x0 t2d 0x1
mov32i $r8 0x0 0xf
mov $r9 c0[0x0] 0xf
tex ll aoffi $r5 $r0 $r8 0x0 t2d 0x1
ipa pass $r12 a[0x7c] 0x0 0x0 0x1
mufu rcp $r12 $r12
ipa $r10 a[0x90] $r12 0x0 0x1
ipa $r11 a[0x94] $r12 0x0 0x1
mov32i $r13 0x3f800000 0xf
tex lb $r6 $r10 $r13 0x1 t2d 0x1
tex $r7 $r10 0x0 0x1 t2d 0x1
mov $r0 $r4 0xf
mov $r1 $r5 0xf
mov $r2 $r6 0xf
mov $r3 $r7 0xf
exit
