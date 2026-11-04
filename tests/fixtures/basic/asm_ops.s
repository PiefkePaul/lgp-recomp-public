// Hand-written AArch64 leaf (own code). Uses flag-setting arithmetic,
// conditional select and a bitfield op so the fixture contains them for sure.
//   uint64_t lgp_asm_mix(uint64_t a, uint64_t b)
    .text
    .global lgp_asm_mix
    .type   lgp_asm_mix, %function
    .p2align 2
lgp_asm_mix:
    adds    x2, x0, x1
    csel    x0, x2, x1, cc
    ubfx    x3, x0, #4, #12
    eor     x0, x0, x3, lsl #1
    ret
    .size   lgp_asm_mix, . - lgp_asm_mix
