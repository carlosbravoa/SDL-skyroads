// Shared little-endian readers and file slurp. The reference design modules each defined a
// private `read_u16`; here they share one helper with identical bounds/EOF
// semantics.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "data/error.hpp"

namespace skyroads::data {

using Bytes = std::vector<uint8_t>;

// Reads a whole file into memory. Throws Error::Io on failure, matching the
// the reference design loaders that surface `std::io::Error` through `From`.
Bytes read_file(const std::string& path);

// Little-endian u16 with the same "u16" EOF context string the reference helpers use.
inline uint16_t read_u16(const Bytes& data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw Error::unexpected_eof("u16");
    }
    return static_cast<uint16_t>(data[offset]) |
           (static_cast<uint16_t>(data[offset + 1]) << 8);
}

// Byte accessor with the same panic-on-out-of-range contract as the reference design indexing;
// only used where the original code already proved the index in range.
inline uint8_t at(const Bytes& data, std::size_t offset) {
    return data.at(offset);
}

} // namespace skyroads::data
