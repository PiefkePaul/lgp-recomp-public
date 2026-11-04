#include "hv_machine.h"

#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace lgp::difftest::hv {
namespace {

constexpr uint64_t kGuestPage = 0x1000;   // stage-1 granule
constexpr uint64_t kHostPage = 0x4000;    // stage-2 / host page on Apple Silicon
constexpr uint64_t kSysBase = kGuestVaLimit;  // system area IPA == VA
constexpr uint64_t kSysSize = 8ull << 20;
constexpr uint64_t kVectorsOff = 0x0;
constexpr uint64_t kStubOff = 0x800;
constexpr uint64_t kTablesOff = 0x1000;
constexpr uint32_t kStubDone = 0x100;  // hvc immediate at the end of the EL1 stub

constexpr uint64_t kCpsrEl0t = 0x0;
constexpr uint64_t kCpsrEl1h = 0x3c5;  // EL1h, DAIF masked

// Exception classes.
constexpr uint32_t kEcSvc64 = 0x15;
constexpr uint32_t kEcHvc64 = 0x16;
constexpr uint32_t kEcBrk64 = 0x3c;

// Descriptor bits (stage 1, 4 KiB granule).
constexpr uint64_t kDescTable = 0x3;
constexpr uint64_t kDescPage = 0x3;
constexpr uint64_t kAttrIdx0 = 0 << 2;
constexpr uint64_t kApEl0Rw = 1ull << 6;
constexpr uint64_t kApEl0Ro = 3ull << 6;
constexpr uint64_t kShInner = 3ull << 8;
constexpr uint64_t kAf = 1ull << 10;
constexpr uint64_t kPxn = 1ull << 53;
constexpr uint64_t kUxn = 1ull << 54;
constexpr uint64_t kAddrMask = 0x0000'FFFF'FFFF'F000ull;

// TCR_EL1: T0SZ=25 (39-bit VA, walk starts at level 1), inner/outer WB WA,
// inner shareable, 4 KiB granule, TTBR1 walks disabled, IPS 36 bits.
constexpr uint64_t kTcr = 25 | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (1ull << 32);
constexpr uint64_t kMair = 0xFF;  // attr0: normal memory, inner/outer write-back
// SCTLR_EL1: RES1 bits of ARMv8.0 (11, 20, 22, 23, 28, 29) plus M, C, I, DZE
// (DC ZVA at EL0), UCT (CTR_EL0 at EL0), nTWI/nTWE (WFI/WFE not trapped) and
// UCI (cache maintenance at EL0). Alignment checks (A, SA0) stay off.
constexpr uint64_t kSctlr = 0x30D0'0800ull | (1ull << 0) | (1ull << 2) | (1ull << 12) | (1ull << 14) |
                            (1ull << 15) | (1ull << 16) | (1ull << 18) | (1ull << 26);
constexpr uint64_t kCpacrFpen = 3ull << 20;  // FP/SIMD not trapped

constexpr uint32_t hvc(uint32_t imm) { return 0xD4000002u | (imm << 5); }

void check(hv_return_t r, const char* what) {
    if (r != HV_SUCCESS) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s failed: 0x%x", what, static_cast<unsigned>(r));
        throw std::runtime_error(buf);
    }
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

hv_vcpu_t vcpu_of(uint64_t v) { return static_cast<hv_vcpu_t>(v); }

uint64_t get_reg(hv_vcpu_t vcpu, hv_reg_t r) {
    uint64_t v = 0;
    check(hv_vcpu_get_reg(vcpu, r, &v), "hv_vcpu_get_reg");
    return v;
}
uint64_t get_sys(hv_vcpu_t vcpu, hv_sys_reg_t r) {
    uint64_t v = 0;
    check(hv_vcpu_get_sys_reg(vcpu, r, &v), "hv_vcpu_get_sys_reg");
    return v;
}
void set_reg(hv_vcpu_t vcpu, hv_reg_t r, uint64_t v) { check(hv_vcpu_set_reg(vcpu, r, v), "hv_vcpu_set_reg"); }
void set_sys(hv_vcpu_t vcpu, hv_sys_reg_t r, uint64_t v) {
    check(hv_vcpu_set_sys_reg(vcpu, r, v), "hv_vcpu_set_sys_reg");
}

// Cancels a running vCPU after a timeout. hv_vcpus_exit() on a vCPU that is
// not running leaves a pending exit; fired() tells the caller to drain it.
class Watchdog {
public:
    Watchdog(hv_vcpu_t vcpu, std::chrono::microseconds timeout)
        : thread_([this, vcpu, timeout] {
              std::unique_lock<std::mutex> lock(m_);
              if (!cv_.wait_for(lock, timeout, [this] { return done_; })) {
                  fired_ = true;
                  hv_vcpu_t v = vcpu;
                  hv_vcpus_exit(&v, 1);
              }
          }) {}
    ~Watchdog() { stop(); }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }
    bool fired() const { return fired_; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    std::atomic<bool> fired_{false};
    std::thread thread_;
};

}  // namespace

const char* Machine::exception_class_name(uint32_t ec) {
    switch (ec) {
    case 0x00: return "undefined instruction";
    case 0x01: return "WFI/WFE trap";
    case 0x07: return "FP/SIMD access trap";
    case 0x0E: return "illegal execution state";
    case 0x15: return "SVC";
    case 0x18: return "system register access trap";
    case 0x20: return "instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "FP exception";
    case 0x3C: return "BRK";
    default: return "exception";
    }
}

Machine::Machine() {
    hv_return_t r = hv_vm_create(nullptr);
    if (r == HV_DENIED)
        throw std::runtime_error(
            "hv_vm_create: HV_DENIED (binary lacks the com.apple.security.hypervisor entitlement?)");
    check(r, "hv_vm_create");

    void* sys = mmap(nullptr, kSysSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (sys == MAP_FAILED)
        throw std::runtime_error("mmap of the system area failed");
    sys_ = static_cast<uint8_t*>(sys);
    check(hv_vm_map(sys_, kSysBase, kSysSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC), "hv_vm_map(sys)");

    // Vector table: entry i is `hvc #i`, so the host learns which vector was
    // taken (8 = synchronous exception from lower EL, AArch64).
    auto* vec = reinterpret_cast<uint32_t*>(sys_ + kVectorsOff);
    for (uint32_t i = 0; i < 16; ++i) {
        vec[i * 0x20] = hvc(i);
        vec[i * 0x20 + 1] = 0x14000000u;  // b . (never reached)
    }
    // EL1 maintenance stub, run after page-table or code changes.
    const uint32_t stub[] = {
        0xD508871Fu,  // tlbi vmalle1
        0xD5033B9Fu,  // dsb ish
        0xD508751Fu,  // ic iallu
        0xD5033B9Fu,  // dsb ish
        0xD5033FDFu,  // isb
        hvc(kStubDone),
    };
    std::memcpy(sys_ + kStubOff, stub, sizeof(stub));

    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t* ex = nullptr;
    check(hv_vcpu_create(&vcpu, &ex, nullptr), "hv_vcpu_create");
    vcpu_ = vcpu;
    exit_ = ex;
    check(hv_vcpu_set_trap_debug_exceptions(vcpu, true), "hv_vcpu_set_trap_debug_exceptions");

    reset_tables();
    set_sys(vcpu, HV_SYS_REG_MAIR_EL1, kMair);
    set_sys(vcpu, HV_SYS_REG_TCR_EL1, kTcr);
    set_sys(vcpu, HV_SYS_REG_TTBR0_EL1, kSysBase + kTablesOff);
    set_sys(vcpu, HV_SYS_REG_VBAR_EL1, kSysBase + kVectorsOff);
    set_sys(vcpu, HV_SYS_REG_CPACR_EL1, kCpacrFpen);
    set_sys(vcpu, HV_SYS_REG_CNTKCTL_EL1, 0);  // counter reads from EL0 trap (not deterministic)
    set_sys(vcpu, HV_SYS_REG_SCTLR_EL1, kSctlr);
    set_sys(vcpu, HV_SYS_REG_SP_EL1, kSysBase + kSysSize);
    set_reg(vcpu, HV_REG_CPSR, kCpsrEl0t);
}

Machine::~Machine() {
    unmap_all();
    hv_vcpu_destroy(vcpu_of(vcpu_));
    hv_vm_unmap(kSysBase, kSysSize);
    hv_vm_destroy();
    munmap(sys_, kSysSize);
}

uint64_t Machine::alloc_table() {
    if (next_table_ + kGuestPage > kSysSize)
        throw std::runtime_error("hv: page-table pool exhausted");
    const uint64_t off = next_table_;
    next_table_ += kGuestPage;
    std::memset(sys_ + off, 0, kGuestPage);
    return kSysBase + off;
}

void Machine::reset_tables() {
    next_table_ = kTablesOff;
    alloc_table();  // level-1 root at kSysBase + kTablesOff
    for (uint64_t off = 0; off < kSysSize; off += kGuestPage)
        *l3_entry(kSysBase + off, true) = (kSysBase + off) | kDescPage | kAttrIdx0 | kShInner | kAf | kUxn;
    dirty_ = true;
}

uint64_t* Machine::l3_entry(uint64_t va, bool create) {
    uint64_t table = kSysBase + kTablesOff;
    for (int level = 1; level <= 2; ++level) {
        const unsigned shift = level == 1 ? 30 : 21;
        auto* t = reinterpret_cast<uint64_t*>(sys_ + (table - kSysBase));
        uint64_t& d = t[(va >> shift) & 0x1FF];
        if ((d & 3) != kDescTable) {
            if (!create)
                return nullptr;
            d = alloc_table() | kDescTable;
        }
        table = d & kAddrMask;
    }
    auto* t = reinterpret_cast<uint64_t*>(sys_ + (table - kSysBase));
    return &t[(va >> 12) & 0x1FF];
}

void Machine::map(uint64_t va, uint64_t size, uint32_t perms) {
    if (va % kGuestPage != 0)
        throw std::invalid_argument("hv: map base " + hex(va) + " not 4 KiB aligned");
    size = (size + kGuestPage - 1) & ~(kGuestPage - 1);
    if (size == 0)
        return;
    if (va + size > kGuestVaLimit || va + size < va)
        throw std::invalid_argument("hv: map " + hex(va) + "+" + hex(size) + " beyond the guest VA limit");

    // Stage 2: back every 16 KiB host page of the range, contiguous runs at once.
    const uint64_t lo = va & ~(kHostPage - 1);
    const uint64_t hi = (va + size + kHostPage - 1) & ~(kHostPage - 1);
    for (uint64_t p = lo; p < hi;) {
        if (pages_.count(p)) {
            p += kHostPage;
            continue;
        }
        uint64_t run_end = p;
        while (run_end < hi && !pages_.count(run_end))
            run_end += kHostPage;
        const uint64_t n = run_end - p;
        void* host = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (host == MAP_FAILED)
            throw std::runtime_error("hv: mmap of guest memory failed");
        check(hv_vm_map(host, p, n, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC), "hv_vm_map");
        chunks_[p] = Chunk{p, n, static_cast<uint8_t*>(host)};
        for (uint64_t q = p; q < run_end; q += kHostPage)
            pages_[q] = static_cast<uint8_t*>(host) + (q - p);
        p = run_end;
    }

    // Stage 1: EL0 pages, never executable at EL1.
    uint64_t attrs = kDescPage | kAttrIdx0 | kShInner | kAf | kPxn;
    attrs |= (perms & kWrite) ? kApEl0Rw : kApEl0Ro;
    if (!(perms & kExec))
        attrs |= kUxn;
    for (uint64_t a = va; a < va + size; a += kGuestPage)
        *l3_entry(a, true) = a | attrs;
    dirty_ = true;
}

void Machine::unmap_all() {
    for (auto& [ipa, c] : chunks_) {
        hv_vm_unmap(c.ipa, c.size);
        munmap(c.host, c.size);
    }
    chunks_.clear();
    pages_.clear();
    reset_tables();
}

bool Machine::is_mapped(uint64_t va) const {
    if (va >= kGuestVaLimit)
        return false;
    const uint64_t* e = const_cast<Machine*>(this)->l3_entry(va, false);
    return e != nullptr && (*e & 3) == kDescPage;
}

uint8_t* Machine::host_page(uint64_t va) const {
    auto it = pages_.find(va & ~(kHostPage - 1));
    return it == pages_.end() ? nullptr : it->second;
}

void Machine::write(uint64_t va, const void* data, uint64_t size) {
    const auto* src = static_cast<const uint8_t*>(data);
    while (size > 0) {
        uint8_t* page = host_page(va);
        if (page == nullptr)
            throw std::out_of_range("hv: write to unmapped guest memory at " + hex(va));
        const uint64_t off = va & (kHostPage - 1);
        const uint64_t n = std::min(size, kHostPage - off);
        std::memcpy(page + off, src, n);
        va += n;
        src += n;
        size -= n;
    }
    dirty_ = true;  // may be code: invalidate the I-cache before the next run
}

void Machine::read(uint64_t va, void* out, uint64_t size) const {
    auto* dst = static_cast<uint8_t*>(out);
    while (size > 0) {
        const uint8_t* page = host_page(va);
        if (page == nullptr)
            throw std::out_of_range("hv: read from unmapped guest memory at " + hex(va));
        const uint64_t off = va & (kHostPage - 1);
        const uint64_t n = std::min(size, kHostPage - off);
        std::memcpy(dst, page + off, n);
        va += n;
        dst += n;
        size -= n;
    }
}

void Machine::set_state(const GuestState& s) {
    const hv_vcpu_t v = vcpu_of(vcpu_);
    for (size_t i = 0; i < s.x.size(); ++i)
        set_reg(v, static_cast<hv_reg_t>(HV_REG_X0 + i), s.x[i]);
    set_reg(v, HV_REG_PC, s.pc);
    set_reg(v, HV_REG_CPSR, kCpsrEl0t | (s.nzcv & 0xF000'0000u));
    set_reg(v, HV_REG_FPCR, s.fpcr);
    set_reg(v, HV_REG_FPSR, s.fpsr);
    set_sys(v, HV_SYS_REG_SP_EL0, s.sp);
    set_sys(v, HV_SYS_REG_TPIDR_EL0, s.tpidr_el0);
    set_sys(v, HV_SYS_REG_TPIDRRO_EL0, s.tpidrro_el0);
    for (size_t i = 0; i < s.v.size(); ++i) {
        hv_simd_fp_uchar16_t q;
        std::memcpy(&q, &s.v[i], sizeof(q));
        check(hv_vcpu_set_simd_fp_reg(v, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), q),
              "hv_vcpu_set_simd_fp_reg");
    }
}

GuestState Machine::state() const {
    const hv_vcpu_t v = vcpu_of(vcpu_);
    GuestState s;
    for (size_t i = 0; i < s.x.size(); ++i)
        s.x[i] = get_reg(v, static_cast<hv_reg_t>(HV_REG_X0 + i));
    s.pc = get_reg(v, HV_REG_PC);
    s.nzcv = static_cast<uint32_t>(get_reg(v, HV_REG_CPSR)) & 0xF000'0000u;
    s.fpcr = static_cast<uint32_t>(get_reg(v, HV_REG_FPCR));
    s.fpsr = static_cast<uint32_t>(get_reg(v, HV_REG_FPSR));
    s.sp = get_sys(v, HV_SYS_REG_SP_EL0);
    s.tpidr_el0 = get_sys(v, HV_SYS_REG_TPIDR_EL0);
    s.tpidrro_el0 = get_sys(v, HV_SYS_REG_TPIDRRO_EL0);
    for (size_t i = 0; i < s.v.size(); ++i) {
        hv_simd_fp_uchar16_t q;
        check(hv_vcpu_get_simd_fp_reg(v, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), &q),
              "hv_vcpu_get_simd_fp_reg");
        std::memcpy(&s.v[i], &q, sizeof(q));
    }
    return s;
}

void Machine::run_el1_stub() {
    const hv_vcpu_t v = vcpu_of(vcpu_);
    const auto* ex = static_cast<const hv_vcpu_exit_t*>(exit_);
    const uint64_t pc = get_reg(v, HV_REG_PC);
    const uint64_t cpsr = get_reg(v, HV_REG_CPSR);
    set_reg(v, HV_REG_PC, kSysBase + kStubOff);
    set_reg(v, HV_REG_CPSR, kCpsrEl1h);
    for (int attempt = 0;; ++attempt) {
        check(hv_vcpu_run(v), "hv_vcpu_run(stub)");
        if (ex->reason == HV_EXIT_REASON_EXCEPTION) {
            const uint32_t ec = static_cast<uint32_t>(ex->exception.syndrome >> 26) & 0x3F;
            if (ec == kEcHvc64 && (ex->exception.syndrome & 0xFFFF) == kStubDone)
                break;
            throw std::runtime_error("hv: EL1 stub raised " + hex(ex->exception.syndrome));
        }
        // A pending cancel from an earlier watchdog, or a timer: run again.
        if (attempt > 16)
            throw std::runtime_error("hv: EL1 stub did not finish");
        set_reg(v, HV_REG_PC, kSysBase + kStubOff);
    }
    set_reg(v, HV_REG_PC, pc);
    set_reg(v, HV_REG_CPSR, cpsr);
    dirty_ = false;
}

Exit Machine::run(std::chrono::microseconds timeout) {
    if (dirty_)
        run_el1_stub();
    const hv_vcpu_t v = vcpu_of(vcpu_);
    const auto* ex = static_cast<const hv_vcpu_exit_t*>(exit_);
    Exit out;
    Watchdog dog(v, timeout);
    for (;;) {
        check(hv_vcpu_run(v), "hv_vcpu_run");
        if (ex->reason == HV_EXIT_REASON_CANCELED) {
            out.kind = Exit::Kind::Timeout;
            out.detail = "timeout";
            break;
        }
        if (ex->reason != HV_EXIT_REASON_EXCEPTION)
            continue;  // virtual timer and the like: nothing to do
        const uint64_t syn = ex->exception.syndrome;
        const uint32_t ec = static_cast<uint32_t>(syn >> 26) & 0x3F;
        if (ec == kEcBrk64) {
            out.kind = Exit::Kind::Brk;
            out.imm = static_cast<uint32_t>(syn & 0xFFFF);
            break;
        }
        if (ec == kEcHvc64 && (syn & 0xFFFF) < 16) {
            // Exception taken to our EL1 vector: back to the EL0 context.
            const uint32_t vector = static_cast<uint32_t>(syn & 0xFFFF);
            out.esr = static_cast<uint32_t>(get_sys(v, HV_SYS_REG_ESR_EL1));
            out.elr = get_sys(v, HV_SYS_REG_ELR_EL1);
            out.far = get_sys(v, HV_SYS_REG_FAR_EL1);
            set_reg(v, HV_REG_PC, out.elr);
            set_reg(v, HV_REG_CPSR, get_sys(v, HV_SYS_REG_SPSR_EL1));
            const uint32_t ec1 = out.esr >> 26;
            if (vector == 8 && ec1 == kEcSvc64) {
                out.kind = Exit::Kind::Svc;
                out.imm = out.esr & 0xFFFF;
                break;
            }
            out.kind = Exit::Kind::Fault;
            out.detail = std::string(exception_class_name(ec1)) + " (ESR " + hex(out.esr) + ") at " +
                         hex(out.elr) + ", FAR " + hex(out.far);
            if (vector != 8)
                out.detail += ", vector " + std::to_string(vector) + " (not from EL0: harness bug)";
            break;
        }
        out.kind = Exit::Kind::Fault;
        out.detail = std::string("exit to host: ") + exception_class_name(ec) + " (syndrome " + hex(syn) +
                     "), VA " + hex(ex->exception.virtual_address) + ", IPA " +
                     hex(ex->exception.physical_address) + ", PC " + hex(get_reg(v, HV_REG_PC));
        break;
    }
    dog.stop();
    if (dog.fired() && out.kind != Exit::Kind::Timeout)
        dirty_ = true;  // the stub run drains the pending cancel
    return out;
}

RunResult HvExecutor::run(const TestCase& tc) {
    Machine& m = machine_;
    m.unmap_all();
    for (const MemoryRegion& r : tc.memory) {
        m.map(r.base, r.bytes.size(), r.perms);
        if (!r.bytes.empty())
            m.write(r.base, r.bytes.data(), r.bytes.size());
    }
    m.set_state(tc.initial);

    RunResult result;
    const auto budget = std::chrono::microseconds(
        std::max<uint64_t>(100'000, tc.max_instructions / 1000));
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        const Exit e = m.run(std::max(left, std::chrono::microseconds(1)));
        if (e.kind == Exit::Kind::Brk) {
            if (e.imm == kSentinelBrk) {
                result.outcome = Outcome::Sentinel;
            } else {
                result.outcome = Outcome::Fault;
                result.detail = "BRK #" + hex(e.imm) + " at " + hex(m.state().pc);
            }
            break;
        }
        if (e.kind == Exit::Kind::Timeout) {
            result.outcome = Outcome::InstructionLimit;
            result.detail = e.detail;
            break;
        }
        if (e.kind == Exit::Kind::Fault) {
            result.outcome = Outcome::Fault;
            result.detail = e.detail;
            break;
        }
        // SVC: x0..x7 through the test case's handler, as in the Unicorn executor.
        GuestState s = m.state();
        SvcRecord rec;
        rec.imm = e.imm;
        for (size_t i = 0; i < rec.args.size(); ++i)
            rec.args[i] = s.x[i];
        rec.results = rec.args;
        if (rec.imm == kSvcExitProcess) {
            result.svcs.push_back(rec);
            result.outcome = Outcome::ExitProcess;
            break;
        }
        if (tc.svc) {
            try {
                tc.svc(rec.imm, rec.results);
            } catch (const std::exception& ex) {
                result.outcome = Outcome::Fault;
                result.detail = std::string("SVC handler threw: ") + ex.what();
                break;
            }
        } else {
            rec.results[0] = 0;
        }
        for (size_t i = 0; i < rec.results.size(); ++i)
            s.x[i] = rec.results[i];
        m.set_state(s);
        result.svcs.push_back(rec);
    }

    result.final = m.state();
    for (const MemoryRegion& r : tc.memory) {
        if (!(r.perms & kWrite))
            continue;
        MemoryRegion out{r.base, std::vector<uint8_t>(r.bytes.size()), r.perms};
        if (!out.bytes.empty())
            m.read(r.base, out.bytes.data(), out.bytes.size());
        result.memory.push_back(std::move(out));
    }
    return result;
}

}  // namespace lgp::difftest::hv
