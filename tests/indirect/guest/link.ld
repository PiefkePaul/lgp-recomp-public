/* NRO-shaped layout for the P2.5 test program: three page-aligned load
   segments (text RX, ro R, data RW + bss), as elf2nro.py expects. */
OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);
    ro   PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dyn  PT_DYNAMIC;
}

SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.crt0)) *(.text .text.*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :ro
    .eh_frame_hdr : { __eh_frame_hdr_start = .; *(.eh_frame_hdr) __eh_frame_hdr_end = .; } :ro
    .eh_frame : { KEEP(*(.eh_frame)) } :ro
    .dynsym : { *(.dynsym) } :ro
    .dynstr : { *(.dynstr) } :ro
    .gnu.hash : { *(.gnu.hash) } :ro
    .hash : { *(.hash) } :ro
    .rela.dyn : { *(.rela.dyn) *(.rela.*) } :ro
    .relr.dyn : { *(.relr.dyn) } :ro

    . = ALIGN(0x1000);
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got : { *(.got) *(.got.plt) } :data
    .data : { *(.data .data.*) } :data
    .bss : {
        __bss_start__ = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end__ = .;
    } :data

    /DISCARD/ : { *(.comment) *(.note.*) *(.interp) }
}
