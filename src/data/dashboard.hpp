// Part of the SkyRoads SDL port
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "data/byteio.hpp"
#include "data/image.hpp"

namespace skyroads::data {

// Index 0 is transparent; 1/2 are the two purple HUD shades.
inline const std::array<RgbColor, 3>& dashboard_colors() {
    static const std::array<RgbColor, 3> colors = {
        RgbColor(0, 0, 0), RgbColor(97, 0, 93), RgbColor(113, 0, 101)};
    return colors;
}

struct HudFragment {
    uint16_t position = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t width = 0;
    uint8_t height = 0;
    Bytes pixels;
};

struct HudFragmentPack {
    std::size_t source_len = 0;
    uint16_t header_words = 0;
    std::vector<HudFragment> fragments;
    std::size_t fragment_count() const { return fragments.size(); }
};

HudFragmentPack load_dashboard_dat_bytes(const Bytes& data);
HudFragmentPack load_dashboard_dat_path(const std::string& path);

} // namespace skyroads::data
