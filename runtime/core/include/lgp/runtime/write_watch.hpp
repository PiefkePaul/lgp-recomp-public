// Write watchpoints for diagnosis: who writes a guest address? Every guest
// store into a watched range is logged with the stored bytes and the pc/lr of
// the guest thread ("watch: 0x… size N = 0x… pc 0x… lr 0x…").
//
// Needs a build with LGP_GUEST_MEM_WRITE_WATCH (stores check the page flags);
// without it add_write_watch() fails. The pc and lr in the log are those of the
// last sync point of the lifted code; with lgp_lift --precise-fault-pc they are
// exact (the writing instruction, the caller's return address).
//
// Seen are stores of lifted code (STR/STP/vector stores, exclusive and atomic
// stores), not writes of HLE services or the GPU into guest memory.
// Boot program: LGP_WATCH_WRITE=VA[:LEN],… (LEN bytes, default 8), filters
// LGP_WATCH_SKIP_ZERO=1 (stores of only zero bytes), LGP_WATCH_FROM_FRAME=N
// (nothing before present N), LGP_WATCH_MAX_LINES=N (default 1000).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lgp::rt {

class GuestMemory;

struct WriteWatch {
    uint64_t va = 0;
    uint64_t size = 8;
};

// True if the runtime was built with LGP_GUEST_MEM_WRITE_WATCH.
bool write_watch_build();

// Watches [va, va + size). The address need not be mapped yet (heap). False
// and *error without LGP_GUEST_MEM_WRITE_WATCH or outside the address space.
bool add_write_watch(GuestMemory& memory, uint64_t va, uint64_t size, std::string* error);
// Removes every watchpoint.
void clear_write_watches(GuestMemory& memory);
// Stores logged so far (the log stops after write_watch_max_lines()).
uint64_t write_watch_hits();
inline constexpr uint64_t kWriteWatchMaxLines = 1000;

// Filters for busy fields (a buffer the game clears every frame): stores whose
// bytes are all zero are not logged; at most max_lines lines (0 = default).
void set_write_watch_filter(bool skip_zero, uint64_t max_lines);
uint64_t write_watch_max_lines();
// Unarmed, watched stores are not logged (the pages stay trapped). Armed by
// default; the boot program disarms until LGP_WATCH_FROM_FRAME.
void set_write_watch_armed(bool armed);
bool write_watch_armed();

// "VA[:LEN][,VA[:LEN]...]", numbers 0x… or decimal. False and *error otherwise.
bool parse_write_watches(const std::string& spec, std::vector<WriteWatch>* out, std::string* error);

}  // namespace lgp::rt
