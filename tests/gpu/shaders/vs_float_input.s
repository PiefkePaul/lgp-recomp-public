// Vertex shader passing generic input 1 through as float: out0 = in1. With
// --int-input 1=<kind> the backend's conversions (scaled integers, packed
// 10:10:10:2 words) are visible in the output. Own test shader.
.stage vertex
.in 0 xyzw
.in 1 xyzw
.out 0 xyzw
ld b128 $r0 a[0x80] 0x0
ld b128 $r4 a[0x90] 0x0
st b128 a[0x70] $r0 0x0
st b128 a[0x80] $r4 0x0
exit
