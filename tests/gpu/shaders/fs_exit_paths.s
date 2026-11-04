// Fragment shader whose SSY is never popped: both paths of the branch end
// in EXIT. red 0.25 left half, 0.75 right half. Own test shader.
.stage fragment
.in 1 perspective xy
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r4 a[0x90] $r8 0x0 0x1
mov32i $r6 0x3f000000 0xf
mov32i $r1 0x0 0xf
mov32i $r2 0x0 0xf
mov32i $r3 0x3f800000 0xf
fsetp lt and $p0 0x1 $r4 $r6 0x1
ssy @join
$p0 bra @left
mov32i $r0 0x3f400000 0xf
exit
left:
mov32i $r0 0x3e800000 0xf
exit
join:
exit
