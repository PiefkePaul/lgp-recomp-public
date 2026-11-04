// HLE for the movie:: API of the multimedia module (ADR-0005: subsdk1 is not
// recompiled, its movie:: imports are answered on the host).
//
// movie:: works like Android's MediaExtractor/MediaCodec (protocol as
// measured on LGP: docs/analysis/14-titelvideo-movie-hle.md):
//   Extractor  opens a container from a path (WebM here), lists its tracks
//              as MediaData key/value sets and hands out compressed samples;
//   Decoder    takes samples into numbered input buffers, returns pictures
//              through numbered output buffers and signals the game's
//              nn::os events when buffers become available;
//   Buffer     a (data, capacity, offset, size) view; MediaData a key/value set.
// The HLE keeps all state on the host, keyed by the guest address of the
// game's object: the classes with virtual methods get a vtable of host
// thunks, the plain functions are host shims (import_shims.cpp). Pictures
// come from platform::IVideo (FFmpeg on the desktop); without a backend the
// video plays as black pictures of the right size and length.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/platform/storage.hpp"
#include "lgp/platform/video.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/system.hpp"

namespace lgp::boot {

using MovieHandler = std::function<void(CpuState*)>;

class MovieHle {
public:
    static MovieHle& instance();
    // data_va: mapped RW page for the vtables, followed by kAllocPages mapped
    // RW pages for the library's own allocations; vthunk_base: dispatch keys for
    // the virtual methods (kClasses * kSlots * 4 bytes, never mapped).
    void reset(rt::Kernel& kernel, uint64_t data_va, uint64_t vthunk_base, std::vector<LiftedEntry>* host);
    // Where videos come from (the game storage: romfs/ or romfs.bin) and who
    // decodes them (nullptr: black pictures). Both must outlive the process.
    // audio_tracks: also offer the videos' Vorbis sound tracks (decoded on
    // the host with libvorbis to 16-bit PCM); off, the game sees video only.
    void configure(platform::IStorage* game, platform::IVideo* video, bool audio_tracks = false);
    // Drops every open video, decoder and file (romfs.bin) at the end of a
    // run, on the calling thread. In the browser the static state would
    // otherwise be destroyed by exit() on the main thread, where closing an
    // OPFS file waits for the OPFS thread and never returns (Safari).
    void release();
    // Handler for an undefined import, empty if it is not a movie:: function.
    MovieHandler handler(const std::string& name);
    // Guest functions the HLE calls (from a guest thread, inside a movie::
    // call): call(ctx, va) runs guest code at va with the current registers.
    using GuestCall = std::function<bool(CpuState*, uint64_t)>;
    void set_guest(GuestCall call, uint64_t signal_event_va) {
        call_ = std::move(call);
        signal_event_va_ = signal_event_va;
    }
    // nn::os::SignalEvent(event) from the current guest thread.
    void signal_event(CpuState* ctx, uint64_t event);

    // Videos (not sound tracks) that started decoding / handed their end
    // marker to the game so far; for scripted input (--press movie-end).
    static uint32_t video_starts();
    static uint32_t video_ends();
    // One line per open extractor and decoder (sample position, buffers per
    // state, last pts in and out); "busy" if the HLE's lock is held. For
    // hang reports, from any host thread.
    static std::vector<std::string> report();

    static constexpr size_t kClasses = 2;  // Extractor, Decoder
    static constexpr size_t kSlots = 64;   // virtual methods per class
    // Mapped RW pages behind data_va's page for memory the movie library
    // allocates itself (Buffer(size_t capacity)).
    static constexpr uint64_t kAllocPages = 32;

private:
    GuestCall call_;
    uint64_t signal_event_va_ = 0;
};

}  // namespace lgp::boot
