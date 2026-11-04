// Fragment shader: flat input 0 scaled to RGBA8 range, (x, y, z) / 255 and
// -w / 255 (shows vs_int_input's values in a colour target). Own test shader.
.stage fragment
.in 0 constant xyzw
.target 0 xyzw
ipa constant $r0 a[0x80] 0x0 0x0 0x1
ipa constant $r1 a[0x84] 0x0 0x0 0x1
ipa constant $r2 a[0x88] 0x0 0x0 0x1
ipa constant $r3 a[0x8c] 0x0 0x0 0x1
fmul32i $r0 $r0 0x3b808081
fmul32i $r1 $r1 0x3b808081
fmul32i $r2 $r2 0x3b808081
fmul32i $r3 $r3 0xbb808081
exit
