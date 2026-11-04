// Fragment shader with nested if/else (SSY/SYNC), a loop left by two
// predicated BRKs (PBK) and a loop whose if/else ends in BRK on both paths.
// red: 0.25 left half, else 0.5 top / 0.75 bottom; green: steps of 0.125
// until >= v, at most 8; blue: after 3 rounds 0.25 left half, else 0.5.
// Own test shader.
.stage fragment
.in 1 perspective xy
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r4 a[0x90] $r8 0x0 0x1
ipa $r5 a[0x94] $r8 0x0 0x1
mov32i $r6 0x3f000000 0xf
fsetp lt and $p0 0x1 $r4 $r6 0x1
ssy @join1
$p0 bra @left
fsetp lt and $p1 0x1 $r5 $r6 0x1
ssy @join2
$p1 bra @inner
mov32i $r0 0x3f400000 0xf
sync
inner:
mov32i $r0 0x3f000000 0xf
sync
join2:
sync
left:
mov32i $r0 0x3e800000 0xf
sync
join1:
mov32i $r1 0x0 0xf
mov32i $r7 0x0 0xf
mov32i $r9 0x8 0xf
pbk @after
loop:
fadd $r1 $r1 0x3e000000
iadd32i $r7 $r7 0x1
fsetp ge and $p2 0x1 $r1 $r5 0x1
$p2 brk
isetp ge and $p3 0x1 $r7 $r9 0x1
$p3 brk
bra @loop
after:
mov32i $r10 0x0 0xf
mov32i $r12 0x3 0xf
pbk @after2
loop2:
iadd32i $r10 $r10 0x1
isetp lt and $p4 0x1 $r10 $r12 0x1
$p4 bra @loop2
$p0 bra @left2
mov32i $r2 0x3f000000 0xf
brk
left2:
mov32i $r2 0x3e800000 0xf
brk
after2:
mov32i $r3 0x3f800000 0xf
exit
