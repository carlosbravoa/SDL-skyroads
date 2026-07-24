#include "data/level.hpp"

#include <cmath>
#include <utility>

namespace skyroads::data {
namespace {

constexpr double EMPTY_COLLISION_MIN_Y = 0x1E80 / 128.0;
constexpr double EMPTY_COLLISION_MAX_Y = 80.0;
constexpr double TUNNEL_ENTRY_MIN_Y = 0x2180 / 128.0;
constexpr double TUNNEL_BASE_Y = 68.0;
constexpr double X_OFFSET = 95.0;
constexpr double PROBE_RADIUS_X = 14.0;

constexpr uint8_t TUNNEL_CEILS[38] = {
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1E, 0x1E, 0x1E, 0x1D,
    0x1D, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x16, 0x14, 0x12, 0x11, 0x0E,
};
constexpr uint8_t TUNNEL_LOWS[30] = {
    0x10, 0x10, 0x10, 0x10, 0x0F, 0x0E, 0x0D, 0x0B, 0x08, 0x07,
    0x06, 0x05, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

TouchEffect effect_for_color_index(uint8_t index) {
    switch (index) {
        case 10: return TouchEffect::Accelerate;
        case 12: return TouchEffect::Kill;
        case 9: return TouchEffect::RefillOxygen;
        case 8: return TouchEffect::Slide;
        case 2: return TouchEffect::Decelerate;
        default: return TouchEffect::None;
    }
}

LevelCell cell_from_descriptor(uint16_t raw_descriptor) {
    const uint8_t color_raw = static_cast<uint8_t>(raw_descriptor & 0x00FF);
    const uint8_t flags = static_cast<uint8_t>(raw_descriptor >> 8);
    const uint8_t color_index_low = color_raw & 0x0F;
    const uint8_t color_index_high = color_raw >> 4;

    std::optional<uint16_t> cube_height;
    switch (flags & 0x06) {
        case 0x00: break;
        case 0x02: cube_height = 100; break;
        case 0x04: cube_height = 120; break;
        default:
            throw Error::invalid_format("unexpected cube height flag bits");
    }

    LevelCell cell;
    cell.raw_descriptor = raw_descriptor;
    cell.color_index_low = color_index_low;
    cell.color_index_high = color_index_high;
    cell.flags = flags;
    cell.has_tunnel = (flags & 0x01) != 0;
    cell.has_tile = color_index_low > 0;
    cell.tile_effect = effect_for_color_index(color_index_low);
    cell.cube_height = cube_height;
    cell.cube_effect = effect_for_color_index(color_index_high);
    return cell;
}

// Returns {distance_from_center, var_a}. the reference design `%` on doubles is fmod semantics.
std::pair<double, double> distance_from_center(double x_pos) {
    double distance = 23.0 - std::fmod(x_pos - 49.0, 46.0);
    double var_a = -46.0;
    if (distance < 0.0) {
        distance = 1.0 - distance;
        var_a = -var_a;
    }
    return {distance, var_a};
}

bool is_inside_tile_y(double y_pos, double distance_from_center,
                      const LevelCell& cell) {
    const double distance_index_f = std::round(distance_from_center);
    if (distance_index_f > 37.0) {
        return false;
    }
    const std::size_t distance_index = static_cast<std::size_t>(distance_index_f);
    const double y2 = y_pos - TUNNEL_BASE_Y;
    const bool has_cube = cell.cube_height.has_value();
    if (cell.has_tunnel && !has_cube) {
        return y2 > static_cast<double>(TUNNEL_LOWS[distance_index]) &&
               y2 < static_cast<double>(TUNNEL_CEILS[distance_index]);
    }
    if (!cell.has_tunnel && has_cube) {
        return y_pos < static_cast<double>(*cell.cube_height);
    }
    if (cell.has_tunnel && has_cube) {
        return y2 > static_cast<double>(TUNNEL_LOWS[distance_index]) &&
               y_pos < static_cast<double>(*cell.cube_height);
    }
    return false;
}

bool is_inside_tunnel_y(double y_pos, double distance_from_center,
                        const LevelCell& cell) {
    const double distance_index_f = std::round(distance_from_center);
    if (distance_index_f > 29.0) {
        return false;
    }
    const std::size_t distance_index = static_cast<std::size_t>(distance_index_f);
    const double y2 = y_pos - TUNNEL_BASE_Y;
    return cell.has_tunnel && cell.has_tile &&
           y2 < static_cast<double>(TUNNEL_LOWS[distance_index]) &&
           y_pos >= 80.0;
}

} // namespace

double Level::gravity_acceleration() const {
    return -(std::floor(static_cast<double>(gravity) * 0x1680 / 0x190)) / 128.0;
}

LevelCell Level::cell_at_indices(std::size_t x_index, std::size_t z_index) const {
    if (z_index < cells.size() && x_index < ROAD_COLUMNS) {
        return cells[z_index][x_index];
    }
    return LevelCell::empty();
}

LevelCell Level::get_cell(double x_pos, double /*y_pos*/, double z_pos) const {
    double x = x_pos - X_OFFSET;
    if (!(x >= 0.0 && x <= 322.0)) {
        return LevelCell::empty();
    }

    const double z = std::floor(std::floor(z_pos * 8.0) / 8.0);
    x /= LEVEL_TILE_STRIDE_X;
    const double x_index = std::floor(x);
    if (x_index < 0.0 || z < 0.0) {
        return LevelCell::empty();
    }

    return cell_at_indices(static_cast<std::size_t>(x_index),
                           static_cast<std::size_t>(z));
}

bool Level::is_inside_tile(double x_pos, double y_pos, double z_pos) const {
    const LevelCell left_tile = get_cell(x_pos - PROBE_RADIUS_X, y_pos, z_pos);
    const LevelCell right_tile = get_cell(x_pos + PROBE_RADIUS_X, y_pos, z_pos);

    if (left_tile.is_empty() && right_tile.is_empty()) {
        return false;
    }
    if (y_pos < EMPTY_COLLISION_MAX_Y && y_pos > EMPTY_COLLISION_MIN_Y) {
        return true;
    }
    if (y_pos < TUNNEL_ENTRY_MIN_Y) {
        return false;
    }

    const auto [distance, var_a] = distance_from_center(x_pos);
    const LevelCell center_tile = get_cell(x_pos, y_pos, z_pos);
    if (is_inside_tile_y(y_pos, distance, center_tile)) {
        return true;
    }
    const LevelCell adjacent_tile = get_cell(x_pos + var_a, y_pos, z_pos);
    return is_inside_tile_y(y_pos, 47.0 - distance, adjacent_tile);
}

bool Level::is_inside_tunnel(double x_pos, double y_pos, double z_pos) const {
    const LevelCell left_tile = get_cell(x_pos - PROBE_RADIUS_X, y_pos, z_pos);
    const LevelCell right_tile = get_cell(x_pos + PROBE_RADIUS_X, y_pos, z_pos);

    if (left_tile.is_empty() && right_tile.is_empty()) {
        return false;
    }

    const auto [distance, var_a] = distance_from_center(x_pos);
    const LevelCell center_tile = get_cell(x_pos, y_pos, z_pos);
    if (is_inside_tunnel_y(y_pos, distance, center_tile)) {
        return true;
    }
    const LevelCell adjacent_tile = get_cell(x_pos + var_a, y_pos, z_pos);
    return is_inside_tunnel_y(y_pos, 47.0 - distance, adjacent_tile);
}

Level level_from_road_entry(const RoadEntry& road) {
    Level level;
    level.road_index = road.index;
    level.name = road.index == 0 ? std::string("Demo Level")
                                 : ("Level " + std::to_string(road.index));
    level.gravity = road.gravity;
    level.fuel = road.fuel;
    level.oxygen = road.oxygen;
    level.cells.reserve(road.rows.size());
    for (const RoadRow& row : road.rows) {
        LevelRow cells;
        for (std::size_t column = 0; column < ROAD_COLUMNS; ++column) {
            cells[column] = cell_from_descriptor(row[column]);
        }
        level.cells.push_back(cells);
    }
    return level;
}

std::vector<Level> levels_from_roads_archive(const RoadsArchive& roads) {
    std::vector<Level> levels;
    levels.reserve(roads.roads.size());
    for (const RoadEntry& road : roads.roads) {
        levels.push_back(level_from_road_entry(road));
    }
    return levels;
}

} // namespace skyroads::data
