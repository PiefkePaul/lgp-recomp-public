// Test program for patch sites of game mods (lgp/runtime/patch.hpp): a loop
// whose body has one patch site, then a constant built in code and one read
// from rodata. Unpatched it returns 5+4+3+2+1 + 1 + 0 = 16.
//   patch_site:  the patch in test_patch.cpp adds 100 * x1 instead, which only
//                comes out right if the lifted code wrote the loop's registers
//                back before the host call and reloaded them after it.
//   imm_site:    MOVZ+MOVK pair, set_immediate_patch() on the MOVK.
//   orr_site:    MOV (bitmask immediate) 1920, minus 1920 again: 0 unpatched.
//   ro_value:    poke_guest() target (read-only segment).
    .text
    .global guest_main
    .type guest_main, %function
guest_main:
    mov     x1, #5
    mov     x2, #0
1:
    .global patch_site
patch_site:
    add     x2, x2, x1
    sub     x1, x1, #1
    .global branch_site
branch_site:
    cbnz    x1, 1b
    movz    w3, #1
    .global imm_site
imm_site:
    movk    w3, #0, lsl #16
    adrp    x4, ro_value
    ldr     w4, [x4, :lo12:ro_value]
    .global orr_site
orr_site:
    orr     w5, wzr, #0x780          // 1920 as a bitmask immediate (MOV alias)
    sub     w5, w5, #1920
    add     x0, x2, x3
    add     x0, x0, x4
    add     x0, x0, x5
    ret
    .size guest_main, . - guest_main

    .section .rodata
    .balign 4
    .global ro_value
ro_value:
    .word   0
