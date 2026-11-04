// Fragment shader: texture gather (TLD4) with a constant offset (.AOFFI) and
// with one offset per texel known only at run time (.PTP). Own test shader.
// c0[0x0], c0[0x4] = packed per-texel offsets, 8 bits per component.
.stage fragment
.target 0 xyzw
mov32i $r0 0x3f000000 0xf
mov32i $r1 0x3f000000 0xf
mov32i $r2 0xff01 0xf
tld4 g aoffi $r4 $r0 $r2 0x0 t2d 0x1
mov $r8 c0[0x0] 0xf
mov $r9 c0[0x4] 0xf
tld4 r ptp $r5 $r0 $r8 0x0 t2d 0xe
mov $r0 $r4 0xf
mov $r1 $r5 0xf
mov $r2 $r6 0xf
mov $r3 $r7 0xf
exit
