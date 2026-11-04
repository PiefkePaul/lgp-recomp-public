# AArch64 code in EL0 under Hypervisor.framework (Apple Silicon) as a test oracle

Independent of the game. Applies to every recompiler or emulator that wants to run AArch64 user code (e.g. Switch game
code, ARMv8.0) natively on an Apple Silicon Mac and compare it with its own translation. Implemented in
`tests/difftest/hv/hv_machine.*` (P2.7, ADR-0006), checked on an M1 with macOS 26.

## Summary

1. **Guest code belongs in EL0 with the MMU switched on.** With the MMU off all memory is device memory:
   unaligned accesses, `DC ZVA` and exclusive accesses then behave differently than on the console.
2. **The host builds the page tables itself** (identity mapping VA = IPA, 4 KiB granule) and needs no
   guest kernel. A tiny EL1 part is enough: a vector table of 16 `hvc #n` and a maintenance stub.
3. **Every exception from EL0 arrives at the host as an HVC.** The vector table jumps out immediately via
   `hvc #<vector number>`; the host reads `ESR_EL1`, `ELR_EL1`, `FAR_EL1`, sets `PC = ELR_EL1` and `CPSR = SPSR_EL1` and is
   back in the EL0 context without guest code having to execute an `ERET`.
4. **`BRK` goes directly to the host** (`hv_vcpu_set_trap_debug_exceptions`), PC points at the `BRK`.
5. **The host code sets all registers directly.** EL0 entry = set `CPSR = 0` (EL0t) and `PC`, stack via
   `SP_EL0` as a system register. An `ERET` from a guest stub is not needed.
6. **A run costs a few microseconds**, the VM exit about 0.7 µs. Only resetting and comparing megabytes of guest memory
   per test case is expensive.

## Setup

| Part | Value | Reason |
|---|---|---|
| IPA size | default (36 bits on M1) | guest VAs below 4 GiB plus system area at 60 GiB |
| Stage 2 | `hv_vm_map` in 16 KiB steps | host page size on Apple Silicon; NSO segments are only 4 KiB aligned, hence permissions only in stage 1 |
| `TCR_EL1` | `T0SZ = 25` (39-bit VA, walk from level 1), IRGN/ORGN = WB-WA, SH = inner, TG0 = 4 KiB, `EPD1 = 1`, IPS = 36 bits | one table hierarchy is enough |
| `MAIR_EL1` | Attr0 = `0xFF` (Normal, WB) | all guest memory is normal memory |
| `SCTLR_EL1` | RES1 bits of ARMv8.0 (11, 20, 22, 23, 28, 29) + M, C, I, DZE, UCT, nTWI, nTWE, UCI; A and SA0 off | `DC ZVA`, `CTR_EL0`, cache maintenance and WFI/WFE in EL0 as with Horizon; alignment checks off |
| `CPACR_EL1` | FPEN = 0b11 | FP/SIMD in EL0 without a trap |
| `CNTKCTL_EL1` | 0 | counter reads from EL0 trap (system register trap, EC 0x18), so no nondeterministic value gets into a comparison |
| EL0 page descriptor | AF, SH inner, AttrIdx 0, PXN always; AP = 01 (RW) or 11 (RO); UXN if not executable | EL1 never executes guest pages |
| System area | vectors (`VBAR_EL1`), stub, page table pool; AP = 00 (EL1 only), UXN | invisible to EL0 |

Maintenance stub in EL1: `tlbi vmalle1; dsb ish; ic iallu; dsb ish; isb; hvc #0x100`. It runs before the next
EL0 run as soon as the host has changed tables or guest code (host writes are data-coherent, the
instruction cache is not).

## Exceptions

| Cause | Exit at the host | Evaluation |
|---|---|---|
| `BRK #imm` | EC 0x3C directly (debug trap) | sentinel or error |
| `SVC #imm` from EL0 | EC 0x16 (`hvc #8`, vector "lower EL, synchronous") | `ESR_EL1.EC = 0x15`, immediate in `ESR_EL1[15:0]`, `ELR_EL1` points behind the `SVC` |
| Data/instruction abort, undefined instruction, system register trap | likewise | `ESR_EL1.EC` 0x24 / 0x20 / 0x00 / 0x18, address in `FAR_EL1` |
| Exception in EL1 (vector 0–7) | HVC with vector number < 8 | error in the harness, not in guest code |
| Stage 2 fault | exit with EC 0x20/0x24 to EL2 | IPA without host memory, likewise a harness error |

## Pitfalls

- **Pending abort.** `hv_vcpus_exit()` (time limit from a watchdog thread) on a vCPU that is not running at the moment
  takes effect at the *next* `hv_vcpu_run()`: it returns immediately with `HV_EXIT_REASON_CANCELED`. If the
  watchdog still fired after the exit, the next run has to swallow it (here: the maintenance stub runs then
  anyway and repeats itself on `CANCELED`).
- **One VM per process, vCPU thread binding.** A process may have only one VM, the vCPU is served only by the creating
  thread. Whoever wants to catch crashes of the comparison partner forks before creating the VM and creates it in the child.
- **Signature.** The program needs the entitlement `com.apple.security.hypervisor`; an ad-hoc signature
  (`codesign --sign - --entitlements …`) as a post-build step is enough, without a developer account.
- **16 KiB host pages outside the hypervisor too.** `mprotect`/`mmap(MAP_FIXED)` on macOS arm64 require
  16 KiB alignment. A guest memory model with 4 KiB blocks must zero the edge pages when freeing instead of
  replacing them, because a neighboring block can use the same host page.
- **M1 is not an A57.** What can show up in the comparison: ID and cache registers (`CTR_EL0`, `MIDR`), counters, the
  block size of `DC ZVA` (64 bytes on both) and extensions beyond ARMv8.0 that the M1 executes but the Switch
  does not. Test code may therefore only contain ARMv8.0 instructions (the function comparison picks functions from
  the game, which are by construction).

## Measurements (M1)

- VM exit (`hvc` → host → guest): about 0.74 µs.
- Function comparison with about 13 MB of writable guest memory (data/BSS of a large module, buffers, stack):
  about 6–7 ms per test case for both sides together, almost only copying and comparing memory.

## Sources

- Apple, Hypervisor.framework (`hv_vm_map`, `hv_vcpu_run`, `hv_vcpus_exit`, `hv_vcpu_set_trap_debug_exceptions`):
  <https://developer.apple.com/documentation/hypervisor>
- Arm Architecture Reference Manual (DDI 0487): VMSAv8-64 (descriptors, `TCR_EL1`), `SCTLR_EL1`, exception syndromes (`ESR_ELx`)
- Ryujinx AppleHv backend (MIT, reading reference for EL0 execution with an EL1 stub): Ryubing fork, `src/Ryujinx.Cpu/AppleHv/`
