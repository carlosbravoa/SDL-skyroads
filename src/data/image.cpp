#include "data/image.hpp"

#include <array>
#include <cstring>
#include <utility>

#include "data/compression.hpp"

namespace skyroads::data {
namespace {

// the reference design `u8::saturating_mul(4)` used to widen 6-bit VGA palette values.
uint8_t saturating_mul4(uint8_t value) {
    const unsigned scaled = static_cast<unsigned>(value) * 4u;
    return scaled > 255u ? 255u : static_cast<uint8_t>(scaled);
}

bool starts_with(const Bytes& data, const char* tag) {
    const std::size_t len = std::strlen(tag);
    if (data.size() < len) return false;
    return std::memcmp(data.data(), tag, len) == 0;
}

std::array<uint8_t, 4> read_ident(const Bytes& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw Error::unexpected_eof("image chunk ident");
    }
    return {data[offset], data[offset + 1], data[offset + 2], data[offset + 3]};
}

bool ident_is(const std::array<uint8_t, 4>& ident, const char* tag) {
    return ident[0] == static_cast<uint8_t>(tag[0]) &&
           ident[1] == static_cast<uint8_t>(tag[1]) &&
           ident[2] == static_cast<uint8_t>(tag[2]) &&
           ident[3] == static_cast<uint8_t>(tag[3]);
}

// Returns the palette plus the number of bytes consumed after the 4-byte tag.
std::pair<ImagePalette, std::size_t> parse_cmap(const Bytes& data,
                                                std::size_t offset) {
    if (offset >= data.size()) {
        throw Error::unexpected_eof("CMAP count");
    }
    const std::size_t color_count = data[offset];
    const std::size_t colors_start = offset + 1;
    const std::size_t colors_end = colors_start + color_count * 3;
    const std::size_t aux_end = colors_end + color_count * 2;
    if (aux_end > data.size()) {
        throw Error::unexpected_eof("CMAP payload");
    }

    ImagePalette palette;
    palette.colors.reserve(color_count);
    for (std::size_t index = 0; index < color_count; ++index) {
        const std::size_t base = colors_start + index * 3;
        palette.colors.push_back(RgbColor(saturating_mul4(data[base]),
                                          saturating_mul4(data[base + 1]),
                                          saturating_mul4(data[base + 2])));
    }
    palette.aux_data.assign(data.begin() + colors_end, data.begin() + aux_end);
    return {std::move(palette), 1 + color_count * 5};
}

// Returns the frame plus bytes consumed after the 4-byte tag.
std::pair<ImageFrame, std::size_t> parse_pict(const Bytes& data,
                                              std::size_t offset,
                                              ImagePalette palette) {
    if (offset + 9 > data.size()) {
        throw Error::unexpected_eof("PICT header");
    }

    const uint16_t screen_offset = read_u16(data, offset);
    const uint16_t height = read_u16(data, offset + 2);
    const uint16_t width = read_u16(data, offset + 4);
    const CompressionWidths widths{data[offset + 6], data[offset + 7],
                                   data[offset + 8]};
    const std::size_t pixel_count =
        static_cast<std::size_t>(width < 1 ? 1 : width) *
        static_cast<std::size_t>(height < 1 ? 1 : height);
    DecompressResult decoded =
        decompress_stream(data, offset + 9, pixel_count, widths);
    const uint16_t actual_height = height == 0 ? 1 : height;

    ImageFrame frame;
    frame.offset = screen_offset;
    frame.x_offset = screen_offset % SCREEN_WIDTH;
    frame.y_offset = screen_offset / SCREEN_WIDTH;
    frame.width = width;
    frame.height = actual_height;
    frame.pixels = std::move(decoded.output);
    frame.palette = std::move(palette);
    frame.transparent_zero = true;
    return {std::move(frame), 9 + decoded.consumed};
}

ImageArchive load_image_set_archive(const Bytes& data) {
    std::size_t cursor = 0;
    bool have_palette = false;
    ImagePalette current_palette;
    std::vector<std::vector<ImageFrame>> frames;

    while (cursor < data.size()) {
        const auto chunk = read_ident(data, cursor);
        if (ident_is(chunk, "CMAP")) {
            auto [palette, consumed] = parse_cmap(data, cursor + 4);
            current_palette = std::move(palette);
            have_palette = true;
            cursor += 4 + consumed;
        } else if (ident_is(chunk, "PICT")) {
            if (!have_palette) {
                throw Error::invalid_format("PICT chunk appeared before CMAP");
            }
            auto [frame, consumed] = parse_pict(data, cursor + 4, current_palette);
            frames.push_back({std::move(frame)});
            cursor += 4 + consumed;
        } else {
            throw Error::invalid_format("unexpected image chunk at offset " +
                                        std::to_string(cursor));
        }
    }

    ImageArchive archive;
    archive.source_len = data.size();
    archive.kind = ImageArchiveKind::image_set();
    archive.frames = std::move(frames);
    return archive;
}

ImageArchive load_anim_archive(const Bytes& data) {
    if (data.size() < 10) {
        throw Error::invalid_format("ANIM archive is truncated");
    }
    const uint16_t declared_frame_count = read_u16(data, 4);
    std::size_t cursor = 6;
    if (!ident_is(read_ident(data, cursor), "CMAP")) {
        throw Error::invalid_format(
            "ANIM archive does not contain CMAP after header");
    }
    auto [palette, consumed] = parse_cmap(data, cursor + 4);
    cursor += 4 + consumed;

    std::vector<std::vector<ImageFrame>> frames;
    frames.reserve(declared_frame_count);
    for (std::size_t frame_index = 0; frame_index < declared_frame_count;
         ++frame_index) {
        if (cursor + 2 > data.size()) {
            throw Error::invalid_format("ANIM frame " +
                                        std::to_string(frame_index) +
                                        " is truncated");
        }
        const std::size_t part_count = read_u16(data, cursor);
        cursor += 2;
        std::vector<ImageFrame> fragments;
        fragments.reserve(part_count);
        for (std::size_t p = 0; p < part_count; ++p) {
            if (!ident_is(read_ident(data, cursor), "PICT")) {
                throw Error::invalid_format("ANIM frame " +
                                            std::to_string(frame_index) +
                                            " expected PICT");
            }
            auto [frame, pict_consumed] = parse_pict(data, cursor + 4, palette);
            fragments.push_back(std::move(frame));
            cursor += 4 + pict_consumed;
        }
        frames.push_back(std::move(fragments));
    }

    ImageArchive archive;
    archive.source_len = data.size();
    archive.kind = ImageArchiveKind::animation(declared_frame_count);
    archive.frames = std::move(frames);
    return archive;
}

} // namespace

ImageArchive load_image_archive_bytes(const Bytes& data) {
    if (starts_with(data, "ANIM")) {
        return load_anim_archive(data);
    }
    if (starts_with(data, "CMAP")) {
        return load_image_set_archive(data);
    }
    throw Error::invalid_format("image archive does not begin with CMAP or ANIM");
}

ImageArchive load_image_archive_path(const std::string& path) {
    return load_image_archive_bytes(read_file(path));
}

} // namespace skyroads::data
