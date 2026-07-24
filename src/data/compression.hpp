// Part of the SkyRoads SDL port
//
// The shared SkyRoads LZ bitstream decoder. This is the single most
// fidelity-critical routine in the whole port: every asset format funnels
// through it, so the bit ordering, back-reference math, and EOF behaviour must
// match the reference design (and thus the DOS build) exactly.
#pragma once

#include <cstdint>
#include <optional>

#include "data/byteio.hpp"

namespace skyroads::data {

struct DecompressResult {
    Bytes output;
    std::size_t consumed;
};

struct CompressionWidths {
    uint8_t w1;
    uint8_t w2;
    uint8_t w3;
};

// `expected_size` maps to the reference's `Option<usize>`: when set, decoding stops once
// the output reaches that length; when empty, decoding runs until the bitstream
// is exhausted (an EOF mid-token ends the stream cleanly instead of erroring).
DecompressResult decompress_stream(const Bytes& data,
                                   std::size_t offset,
                                   std::optional<std::size_t> expected_size,
                                   CompressionWidths widths);

} // namespace skyroads::data
