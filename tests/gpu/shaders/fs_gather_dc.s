// Fragment shader: depth-compare gather on a 2D array texture (TLD4.DC with
// the layer in Ra) and an offset outside WGSL's constant range (-8..7), which
// becomes a coordinate shift. Own test shader, compiled but not rendered.
.stage fragment
.target 0 xyzw
mov32i $r0 0x1 0xf
mov32i $r1 0x3f000000 0xf
mov32i $r2 0x3f000000 0xf
mov32i $r4 0xf40c 0xf
mov32i $r5 0x3f000000 0xf
tld4 r aoffi dc $r8 $r0 $r4 0x0 array t2d 0xf
mov $r0 $r8 0xf
mov $r1 $r9 0xf
mov $r2 $r10 0xf
mov $r3 $r11 0xf
exit
