// Part of the SkyRoads SDL port
//
// Level geometry plus the DOS collision probes, in two exactly-equivalent
// forms: the double reference (constants as exact `hex / 0x80` ratios) used by
// the renderer and tests, and the integer fixed-point core the simulation tick
// runs on (the target console has no FPU).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "data/image.hpp"
#include "data/roads.hpp"

namespace skyroads::data {

constexpr double LEVEL_TILE_STRIDE_X = 46.0;
constexpr double LEVEL_CENTER_X = 0x8000 / 128.0;
constexpr double LEVEL_MIN_X = 0x2F80 / 128.0;
constexpr double LEVEL_MAX_X = 0xD080 / 128.0;
constexpr double GROUND_Y = 0x2800 / 128.0;

// The same landmarks in the DOS EXE's own fixed-point units: x/y are 16-bit
// words in 1/128 units ("_128"), z is a 32-bit 16.16 value ("_fp16"). The
// simulation runs entirely on these; the doubles above serve the renderer and
// the reference implementation.
constexpr int32_t LEVEL_CENTER_X_128 = 0x8000;
constexpr int32_t LEVEL_MIN_X_128 = 0x2F80;
constexpr int32_t LEVEL_MAX_X_128 = 0xD080;
constexpr int32_t GROUND_Y_128 = 0x2800;

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
    // The road's 72-entry VGA palette (already scaled 6-bit -> 8-bit). The DOS
    // road renderer indexes this by TREKDAT colour code for exact road/wall hues.
    std::vector<RgbColor> palette;

    std::size_t width() const { return ROAD_COLUMNS; }
    std::size_t length() const { return cells.size(); }
    double gravity_acceleration() const;
    LevelCell cell_at_indices(std::size_t x_index, std::size_t z_index) const;
    LevelCell get_cell(double x_pos, double y_pos, double z_pos) const;
    bool is_inside_tile(double x_pos, double y_pos, double z_pos) const;
    bool is_inside_tunnel(double x_pos, double y_pos, double z_pos) const;

    // Integer fixed-point collision probes for the simulation tick: no
    // float/double operations anywhere inside (the target console has no FPU).
    // x here is 16.16 because will_land_on_tile accumulates x at that
    // precision; a 1/128 caller passes `x_128 * 512`. y is 1/128, z is 16.16.
    // Bit-exact images of the double probes above for on-grid inputs.
    int32_t gravity_acceleration_128() const;
    LevelCell get_cell_fp16(int32_t x_fp16, int32_t z_fp16) const;
    bool is_inside_tile_128(int32_t x_128, int32_t y_128, int32_t z_fp16) const;
    bool is_inside_tunnel_128(int32_t x_128, int32_t y_128,
                              int32_t z_fp16) const;
};

Level level_from_road_entry(const RoadEntry& road);
std::vector<Level> levels_from_roads_archive(const RoadsArchive& roads);

} // namespace skyroads::data
