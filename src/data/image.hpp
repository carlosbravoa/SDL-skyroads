// Part of the SkyRoads SDL port
//
// CMAP/PICT image containers and the ANIM wrapper. `ImageArchiveKind` is a
// payload-carrying the reference design enum; here it is a small tagged struct.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

constexpr uint16_t SCREEN_WIDTH = 320;
constexpr uint16_t SCREEN_HEIGHT = 200;

struct RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    constexpr RgbColor() : r(0), g(0), b(0) {}
    constexpr RgbColor(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
    static constexpr RgbColor make(uint8_t r, uint8_t g, uint8_t b) {
        return RgbColor(r, g, b);
    }

    bool operator==(const RgbColor& o) const {
        return r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const RgbColor& o) const { return !(*this == o); }
    // Total order to mirror the reference's Ord derive (used where colors are keyed).
    bool operator<(const RgbColor& o) const {
        if (r != o.r) return r < o.r;
        if (g != o.g) return g < o.g;
        return b < o.b;
    }
};

struct ImagePalette {
    std::vector<RgbColor> colors;
    Bytes aux_data;
    std::size_t color_count() const { return colors.size(); }
};

struct ImageFrame {
    uint16_t offset = 0;
    uint16_t x_offset = 0;
    uint16_t y_offset = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    Bytes pixels;
    ImagePalette palette;
    // When a PICT is preceded by TWO CMAPs, the game shows the picture in the first
    // one and then fades the hardware palette across to the second (the intro applies
    // it at @0x483a / @0x49a8 and fades with @0x4315). With a single CMAP the two are
    // identical.
    ImagePalette palette_start;
    bool transparent_zero = false;
    std::size_t pixel_count() const { return pixels.size(); }
};

// the reference design: `enum ImageArchiveKind { ImageSet, Animation { declared_frame_count } }`
struct ImageArchiveKind {
    enum class Tag { ImageSet, Animation } tag = Tag::ImageSet;
    uint16_t declared_frame_count = 0; // valid only when tag == Animation

    static ImageArchiveKind image_set() { return ImageArchiveKind{Tag::ImageSet, 0}; }
    static ImageArchiveKind animation(uint16_t count) {
        return ImageArchiveKind{Tag::Animation, count};
    }
    bool operator==(const ImageArchiveKind& o) const {
        return tag == o.tag &&
               (tag != Tag::Animation ||
                declared_frame_count == o.declared_frame_count);
    }
};

struct ImageArchive {
    std::size_t source_len = 0;
    ImageArchiveKind kind;
    // Each frame is a list of fragments (image-set frames hold a single one).
    std::vector<std::vector<ImageFrame>> frames;

    std::size_t frame_count() const { return frames.size(); }
    std::size_t total_fragment_count() const {
        std::size_t total = 0;
        for (const auto& frame : frames) total += frame.size();
        return total;
    }
};

ImageArchive load_image_archive_bytes(const Bytes& data);
ImageArchive load_image_archive_path(const std::string& path);

} // namespace skyroads::data
