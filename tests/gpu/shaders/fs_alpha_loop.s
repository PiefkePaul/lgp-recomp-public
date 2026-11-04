// Fragment shader with control flow: alpha test via KIL (WebGPU has no
// fixed-function alpha test), then a counted loop between SSY/SYNC.
// Own test shader.
.stage fragment
.in 0 perspective xyzw
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r0 a[0x80] $r8 0x0 0x1
ipa $r1 a[0x84] $r8 0x0 0x1
ipa $r2 a[0x88] $r8 0x0 0x1
ipa $r3 a[0x8c] $r8 0x0 0x1
fsetp lt and $p0 0x1 $r3 c0[0x0] 0x1
$p0 kil
mov32i $r4 0x0 0xf
mov32i $r5 0x0 0xf
ssy @after
loop:
isetp ge and $p1 0x1 $r5 c0[0x4] 0x1
$p1 sync
fadd $r4 $r4 0x3e800000
iadd32i $r5 $r5 0x1
bra @loop
after:
fmul $r0 $r0 $r4
fmul $r1 $r1 $r4
fmul $r2 $r2 $r4
exit
