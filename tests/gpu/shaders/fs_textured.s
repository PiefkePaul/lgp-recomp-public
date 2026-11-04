// Fragment shader: perspective-correct colour and uv, one 2D texture
// (TEXS, handle 0x8), output = texel * colour. Own test shader.
.stage fragment
.in 0 perspective xyzw
.in 1 perspective xy
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r0 a[0x90] $r8 0x0 0x1
ipa $r1 a[0x94] $r8 0x0 0x1
texs nodep $r2 $r0 $r0 $r1 0x8 t2d rgba
ipa $r4 a[0x80] $r8 0x0 0x1
ipa $r5 a[0x84] $r8 0x0 0x1
ipa $r6 a[0x88] $r8 0x0 0x1
ipa $r7 a[0x8c] $r8 0x0 0x1
fmul $r0 $r0 $r4
fmul $r1 $r1 $r5
fmul $r2 $r2 $r6
fmul $r3 $r3 $r7
exit
