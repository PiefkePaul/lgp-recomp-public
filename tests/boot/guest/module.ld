/* Shared-object layout of the SDK-shaped boot test modules: three
   page-aligned load segments (text RX, ro R, data RW + bss) from address 0,
   as elf2nso.py expects. PLT and GOT stay separate so DT_JMPREL works.
   Segments on 16 KiB boundaries: the native start (docs/analysis/56) maps
   text R+X and data RW, which must not share a 16 KiB host page (Apple). */
OUTPUT_FORMAT("elf64-littleaarch64")

PHDRS {
    text PT_LOAD FLAGS(5);
    ro   PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dyn  PT_DYNAMIC;
}

SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.crt0)) *(.text .text.*) } :text
    .plt : { *(.plt) *(.iplt) } :text

    . = ALIGN(0x4000);
    .rodata : { *(.rodata .rodata.*) } :ro
    .eh_frame_hdr : { HIDDEN(__eh_frame_hdr_start = .); *(.eh_frame_hdr) HIDDEN(__eh_frame_hdr_end = .); } :ro
    .eh_frame : { KEEP(*(.eh_frame)) } :ro
    .dynsym : { *(.dynsym) } :ro
    .dynstr : { *(.dynstr) } :ro
    .gnu.hash : { *(.gnu.hash) } :ro
    .hash : { *(.hash) } :ro
    .rela.dyn : { *(.rela.dyn) } :ro
    .rela.plt : { *(.rela.plt) } :ro

    . = ALIGN(0x4000);
    .init_array : { KEEP(*(.init_array .init_array.*)) } :data
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data
    .data : { *(.data .data.*) } :data
    .bss : {
        HIDDEN(__bss_start__ = .);
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        HIDDEN(__bss_end__ = .);
    } :data

    /DISCARD/ : { *(.comment) *(.note.*) *(.interp) }
}
