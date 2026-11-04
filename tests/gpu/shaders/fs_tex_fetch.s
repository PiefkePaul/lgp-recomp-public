// Fragment shader: texel fetches (TLDS 2D.LZ and 2D.LL.AOFFI) and size
// queries (TXQ.DIMENSION with levels). Own test shader.
// c0[0x0], c0[0x4] = fetch coordinates (integers).
.stage fragment
.target 0 xyzw
mov $r0 c0[0x0] 0xf
mov $r1 c0[0x4] 0xf
tlds lz 0x0 $r4 $r0 $r1 0x0 t2d r
mov32i $r6 0x2 0xf
mov32i $r7 0x1 0xf
mov32i $r8 0x1 0xf
mov32i $r9 0xff 0xf
tlds ll aoffi 0x0 $r5 $r6 $r8 0x1 t2d r
txq $r10 $r8 dimension 0x1 0x9
mov $r0 $r4 0xf
mov $r1 $r5 0xf
i2f f32 s32 $r2 $r10
i2f f32 s32 $r3 $r11
exit
