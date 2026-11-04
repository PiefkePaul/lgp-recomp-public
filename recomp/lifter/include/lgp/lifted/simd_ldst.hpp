// Advanced SIMD load/store structures (LD1-LD4/ST1-ST4/LDnR) (P2.4).
// Header-only helpers called by lifted code; see simd_common.hpp for lane access.
// Included by lifted.hpp after the runtime header, so lgp_mem_read*/lgp_mem_write* are available.
//
// Accesses happen element by element in the order of the Arm ARM pseudocode
// (no single-copy atomicity beyond the element size is required). The lifter
// passes the consecutive registers (numbers wrap modulo 32) as a list:
// pointers to the destinations for loads, copies of the sources for stores.
// The runtime's memory functions may be macros that use `ctx`, hence the
// parameter name.
#pragma once

#include "lgp/lifted/simd_common.hpp"
#include "lgp/lifted/simd_perm.hpp"

namespace lgp::lifted::simd {

struct VRefs {
    V128* r[4];
};
struct VVals {
    V128 r[4];
};

template <class T>
inline T mem_read(CpuState* ctx, uint64_t va) {
    (void)ctx;
    if constexpr (sizeof(T) == 1) return lgp_mem_read8(va);
    else if constexpr (sizeof(T) == 2) return lgp_mem_read16(va);
    else if constexpr (sizeof(T) == 4) return lgp_mem_read32(va);
    else return lgp_mem_read64(va);
}

template <class T>
inline void mem_write(CpuState* ctx, uint64_t va, T v) {
    (void)ctx;
    if constexpr (sizeof(T) == 1) lgp_mem_write8(va, v);
    else if constexpr (sizeof(T) == 2) lgp_mem_write16(va, v);
    else if constexpr (sizeof(T) == 4) lgp_mem_write32(va, v);
    else lgp_mem_write64(va, v);
}

// LD1 (1-4 registers, rpt = count, selem = 1) and LD2/LD3/LD4 (rpt = 1, selem = 2..4),
// multiple structures. Every element of the destination registers is loaded, so the
// result replaces them completely (Q = 0 clears the upper halves).
template <class T>
inline void ld_multi(CpuState* ctx, uint64_t addr, VRefs t, unsigned selem, unsigned rpt, bool q) {
    V128 regs[4] = {zero(), zero(), zero(), zero()};
    const unsigned n = lanes_of<T>(q);
    uint64_t offs = 0;
    for (unsigned r = 0; r < rpt; ++r)
        for (unsigned e = 0; e < n; ++e)
            for (unsigned s = 0; s < selem; ++s) {
                set_lane<T>(regs[r + s], e, mem_read<T>(ctx, addr + offs));
                offs += sizeof(T);
            }
    for (unsigned i = 0; i < rpt + selem - 1; ++i)
        *t.r[i] = regs[i];
}

// ST1 (1-4 registers) and ST2/ST3/ST4, multiple structures.
template <class T>
inline void st_multi(CpuState* ctx, uint64_t addr, const VVals& t, unsigned selem, unsigned rpt, bool q) {
    const unsigned n = lanes_of<T>(q);
    uint64_t offs = 0;
    for (unsigned r = 0; r < rpt; ++r)
        for (unsigned e = 0; e < n; ++e)
            for (unsigned s = 0; s < selem; ++s) {
                mem_write<T>(ctx, addr + offs, lane<T>(t.r[r + s], e));
                offs += sizeof(T);
            }
}

// LD1-LD4 (single structure): one lane of selem consecutive registers; the other lanes are kept.
template <class T>
inline void ld_single(CpuState* ctx, uint64_t addr, VRefs t, unsigned selem, unsigned index) {
    for (unsigned s = 0; s < selem; ++s)
        set_lane<T>(*t.r[s], index, mem_read<T>(ctx, addr + s * sizeof(T)));
}

// ST1-ST4 (single structure).
template <class T>
inline void st_single(CpuState* ctx, uint64_t addr, const VVals& t, unsigned selem, unsigned index) {
    for (unsigned s = 0; s < selem; ++s)
        mem_write<T>(ctx, addr + s * sizeof(T), lane<T>(t.r[s], index));
}

// LD1R-LD4R: one element per register, replicated to all lanes (Q = 0 clears the upper halves).
template <class T>
inline void ld_rep(CpuState* ctx, uint64_t addr, VRefs t, unsigned selem, bool q) {
    for (unsigned s = 0; s < selem; ++s)
        *t.r[s] = dup<T>(mem_read<T>(ctx, addr + s * sizeof(T)), q);
}

}  // namespace lgp::lifted::simd
