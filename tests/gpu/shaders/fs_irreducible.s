// Fragment shader with an irreducible loop: the left half enters it at B,
// the right half at A. The structurizer gives up and keeps the dispatcher.
// red: +0.25 per block until >= 0.6 after B: 0.75 left half, 1.0 right half.
// Own test shader.
.stage fragment
.in 1 perspective xy
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r4 a[0x90] $r8 0x0 0x1
mov32i $r6 0x3f000000 0xf
mov32i $r7 0x3f19999a 0xf
mov32i $r0 0x0 0xf
mov32i $r1 0x0 0xf
mov32i $r2 0x0 0xf
mov32i $r3 0x3f800000 0xf
fsetp lt and $p0 0x1 $r4 $r6 0x1
$p0 bra @b
a:
fadd $r0 $r0 0x3e800000
b:
fadd $r0 $r0 0x3e800000
fsetp lt and $p1 0x1 $r0 $r7 0x1
$p1 bra @a
exit
