// Fragment shader: one 3D texture (TEXS t3d, handle 0x8) at (u, v) from
// generic 1 and w = red of generic 0; output = texel. Own test shader.
// Hand-encoded from fs_textured (only the TEXS type and Rb differ); the
// decoder test checks the encoding against the disassembly below.
.stage fragment
.in 0 perspective xyzw
.in 1 perspective xy
.target 0 xyzw
ipa pass $r8 a[0x7c] 0x0 0x0 0x1
mufu rcp $r8 $r8
ipa $r0 a[0x90] $r8 0x0 0x1
ipa $r1 a[0x94] $r8 0x0 0x1
ipa $r4 a[0x80] $r8 0x0 0x1
texs nodep $r2 $r0 $r0 $r4 0x8 t3d rgba
exit
