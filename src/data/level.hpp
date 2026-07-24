// Part of the SkyRoads SDL port
//
// Level geometry plus the DOS collision probes. The physics constants are kept
// as exact `hex / 0x80` fixed-point ratios in double precision, matching the
// the reference design reference literal-for-literal.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "data/roads.hpp"

namespace skyroads::data {

constexpr double LEVEL_TILE_STRIDE_X = 46.0;
constexpr double LEVEL_CENTER_X = 0x8000 / 128.0;
constexpr double LEVEL_MIN_X = 0x2F80 / 128.0;
constexpr double LEVEL_MAX_X = 0xD080 / 128.0;
constexpr double GROUND_Y = 0x2800 / 128.0;

enum class TouchEffect {
    None,
    Accelerate,
    Decelerate,
    Kill,
    Slide,
    RefillOxygen,
};

struct LevelCell {
    uint16_t raw_descriptor = 0;
    uint8_t color_index_low = 0;
    uint8_t color_index_high = 0;
    uint8_t flags = 0;
    bool has_tunnel = false;
    bool has_tile = false;
    TouchEffect tile_effect = TouchEffect::None;
    std::optional<uint16_t> cube_height;
    TouchEffect cube_effect = TouchEffect::None;

    static LevelCell empty() { return LevelCell{}; }
    bool is_empty() const {
        return !has_tunnel && !has_tile && !cube_height.has_value();
    }
    bool operator==(const LevelCell& o) const {
        return raw_descriptor == o.raw_descriptor &&
               color_index_low == o.color_index_low &&
               color_index_high == o.color_index_high && flags == o.flags &&
               has_tunnel == o.has_tunnel && has_tile == o.has_tile &&
               tile_effect == o.tile_effect && cube_height == o.cube_height &&
               cube_effect == o.cube_effect;
    }
};

using LevelRow = std::array<LevelCell, ROAD_COLUMNS>;

struct Level {
    std::size_t road_index = 0;
    std::string name;
    uint16_t gravity = 0;
    uint16_t fuel = 0;
    uint16_t oxygen = 0;
    std::vector<LevelRow> cells;

    std::size_t width() const { return ROAD_COLUMNS; }
    std::size_t length() const { return cells.size(); }
    double gravity_acceleration() const;
    LevelCell cell_at_indices(std::size_t x_index, std::size_t z_index) const;
    LevelCell get_cell(double x_pos, double y_pos, double z_pos) const;
    bool is_inside_tile(double x_pos, double y_pos, double z_pos) const;
    bool is_inside_tunnel(double x_pos, double y_pos, double z_pos) const;
};

Level level_from_road_entry(const RoadEntry& road);
std::vector<Level> levels_from_roads_archive(const RoadsArchive& roads);

} // namespace skyroads::data
