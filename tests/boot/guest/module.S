// Module header of the SDK-shaped boot test modules: a branch at +0 and the
// MOD0 offset at +4, as every Horizon module has them (switchbrew "NSO").
// The boot program never runs this code; rtld's job is done on the host.
    .section .text.crt0, "ax", %progbits
    .global __lgp_module_start
    .hidden __lgp_module_start
    .type __lgp_module_start, %function
__lgp_module_start:
    b       1f
    .word   __lgp_mod0 - __lgp_module_start
1:
    ret
    .size __lgp_module_start, . - __lgp_module_start

    .section .rodata.mod0, "a", %progbits
    .balign 4
__lgp_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __lgp_mod0
    .word   __bss_start__ - __lgp_mod0
    .word   __bss_end__ - __lgp_mod0
    .word   __eh_frame_hdr_start - __lgp_mod0
    .word   __eh_frame_hdr_end - __lgp_mod0
    .word   0                         // module object (rtld's, unused here)

// Supervisor calls used by both modules (hidden, each module has its own copy).
    .text
    .global lgp_svc_output_debug_string
    .hidden lgp_svc_output_debug_string
    .type lgp_svc_output_debug_string, %function
lgp_svc_output_debug_string:
    svc     #0x27
    ret
    .size lgp_svc_output_debug_string, . - lgp_svc_output_debug_string

    .global lgp_svc_query_memory
    .hidden lgp_svc_query_memory
    .type lgp_svc_query_memory, %function
lgp_svc_query_memory:
    svc     #0x06
    ret
    .size lgp_svc_query_memory, . - lgp_svc_query_memory

    .global lgp_svc_exit_process
    .hidden lgp_svc_exit_process
    .type lgp_svc_exit_process, %function
lgp_svc_exit_process:
    svc     #0x07
    b       lgp_svc_exit_process
    .size lgp_svc_exit_process, . - lgp_svc_exit_process
