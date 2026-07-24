#include "data/compression.hpp"

namespace skyroads::data {
namespace {

// Mirrors the reference `BitReader`: MSB-first within each byte.
class BitReader {
public:
    BitReader(const Bytes& data, std::size_t offset)
        : data_(data), byte_offset_(offset), bit_offset_(0) {}

    // Reads `count` bits MSB-first. Throws UnexpectedEof past the buffer, which
    // the caller turns into a clean stream end when size is unbounded.
    uint32_t read_bits(uint8_t count) {
        uint32_t value = 0;
        for (uint8_t i = 0; i < count; ++i) {
            if (byte_offset_ >= data_.size()) {
                throw Error::unexpected_eof("compressed bitstream");
            }
            const uint8_t bit =
                (data_[byte_offset_] >> (7 - bit_offset_)) & 1u;
            value = (value << 1) | static_cast<uint32_t>(bit);
            bit_offset_ += 1;
            if (bit_offset_ == 8) {
                bit_offset_ = 0;
                byte_offset_ += 1;
            }
        }
        return value;
    }

    std::size_t bytes_consumed(std::size_t start_offset) const {
        const std::size_t extra = bit_offset_ != 0 ? 1 : 0;
        return (byte_offset_ + extra) - start_offset;
    }

private:
    const Bytes& data_;
    std::size_t byte_offset_;
    uint8_t bit_offset_;
};

void copy_from_history(Bytes& output, std::size_t distance, std::size_t count,
                       std::size_t limit) {
    if (distance == 0 || distance > output.size()) {
        throw Error::invalid_format("invalid back-reference distance " +
                                    std::to_string(distance));
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (output.size() >= limit) {
            break;
        }
        const std::size_t source_index = output.size() - distance;
        output.push_back(output[source_index]);
    }
}

} // namespace

DecompressResult decompress_stream(const Bytes& data, std::size_t offset,
                                   std::optional<std::size_t> expected_size,
                                   CompressionWidths widths) {
    const uint8_t width1 = widths.w1;
    const uint8_t width2 = widths.w2;
    const uint8_t width3 = widths.w3;
    BitReader reader(data, offset);
    Bytes output;

    while (true) {
        if (expected_size && output.size() >= *expected_size) {
            break;
        }

        try {
            uint32_t prefix = reader.read_bits(1);
            if (prefix == 0) {
                const std::size_t distance =
                    static_cast<std::size_t>(reader.read_bits(width2)) + 2;
                const std::size_t count =
                    static_cast<std::size_t>(reader.read_bits(width1)) + 2;
                const std::size_t limit =
                    expected_size ? *expected_size : output.size() + count;
                copy_from_history(output, distance, count, limit);
                continue;
            }

            prefix = reader.read_bits(1);
            if (prefix == 0) {
                const std::size_t distance =
                    static_cast<std::size_t>(reader.read_bits(width3)) + 2 +
                    (static_cast<std::size_t>(1) << width2);
                const std::size_t count =
                    static_cast<std::size_t>(reader.read_bits(width1)) + 2;
                const std::size_t limit =
                    expected_size ? *expected_size : output.size() + count;
                copy_from_history(output, distance, count, limit);
                continue;
            }

            output.push_back(static_cast<uint8_t>(reader.read_bits(8)));
        } catch (const Error& error) {
            // Unbounded streams end when the bitstream runs dry; every other
            // error (and any EOF on a sized stream) propagates, matching the
            // the reference design match arm.
            if (error.kind() == Error::Kind::UnexpectedEof && !expected_size) {
                break;
            }
            throw;
        }
    }

    return DecompressResult{std::move(output), reader.bytes_consumed(offset)};
}

} // namespace skyroads::data
