// Minimal NRO start code for the benchmark program (bench/lifter) (no libnx, no dynamic loader).
// Layout of the first 0x80 bytes follows the homebrew NRO convention: a branch
// at +0, the MOD0 offset at +4, the NRO header at +0x10 (filled in by
// elf2nro.py). The recompiler's loader applies the relocations, so the program
// starts with a relocated image and needs no self-relocation.
    .section .text.crt0, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    b       1f
    .word   __lgp_mod0 - _start
    .space  0x78
1:
    // x0 = kernel, x1 = iterations (set by the harness), sp = top of the stack region.
    bl      bench_main
    brk     #0x4C47                  // sentinel: end of the test (tests/difftest/oracle.h)
    .size _start, . - _start

    .section .rodata.mod0, "a", %progbits
    .balign 4
__lgp_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __lgp_mod0
    .word   __bss_start__ - __lgp_mod0
    .word   __bss_end__ - __lgp_mod0
    .word   __eh_frame_hdr_start - __lgp_mod0
    .word   __eh_frame_hdr_end - __lgp_mod0
    .word   0                         // module object (unused)
