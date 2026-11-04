// NSO / NRO loader: file bytes -> Module (unlinked, base 0, relocations not applied).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lgp/recomp/module.hpp"

namespace lgp::recomp {

struct LoadOptions {
    bool verify_hashes = true;     // NSO segment SHA-256
    bool parse_eh_frame = true;    // .eh_frame_hdr -> fde_starts / fde_ranges
    uint64_t max_image_size = 512ull << 20;
};

// Raw LZ4 block (no frame), decompressed size known from the NSO header.
std::vector<uint8_t> lz4_block_decompress(std::span<const uint8_t> src, size_t out_size);

bool is_nso(std::span<const uint8_t> bytes);
bool is_nro(std::span<const uint8_t> bytes);

Module load_nso(std::span<const uint8_t> bytes, const std::string& name, const LoadOptions& opt = {});
Module load_nro(std::span<const uint8_t> bytes, const std::string& name, const LoadOptions& opt = {});
// Detects the format by magic.
Module load_module(std::span<const uint8_t> bytes, const std::string& name, const LoadOptions& opt = {});
Module load_module_file(const std::string& path, const LoadOptions& opt = {});

std::vector<uint8_t> read_file(const std::string& path);

// Parses .eh_frame_hdr + FDEs inside the (unlinked) image. Exposed for tests.
void parse_eh_frame(Module& m);

}  // namespace lgp::recomp
