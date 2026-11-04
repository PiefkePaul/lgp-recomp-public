// Fragment shader: LOD query (TMML) from the coordinate derivatives,
// relative and clamped LOD as 8.8 fixed point. Own test shader.
.stage fragment
.in 1 perspective xy
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r4 a[0x90] $r8 0x0 0x1
ipa $r5 a[0x94] $r8 0x0 0x1
tmml lod $r6 $r4 0x0 t2d 0x3
i2f f32 s32 $r0 $r6
i2f f32 s32 $r1 $r7
mov32i $r2 0x0 0xf
mov32i $r3 0x3f800000 0xf
exit
