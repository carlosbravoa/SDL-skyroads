#include "renderer/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "core/planner.hpp"
#include "data/dashboard.hpp"
#include "data/trekdat.hpp"

namespace skyroads::renderer {

using skyroads::core::renderer_row_state;
using skyroads::core::ShipState;
using skyroads::data::dashboard_colors;
using skyroads::data::GROUND_Y;
using skyroads::data::LEVEL_CENTER_X;
using skyroads::data::LEVEL_TILE_STRIDE_X;
using skyroads::data::ROAD_COLUMNS;
using skyroads::data::SCREEN_HEIGHT;
using skyroads::data::SCREEN_WIDTH;
using skyroads::data::TouchEffect;
using skyroads::data::TrekdatCellPointers;
using skyroads::data::TrekdatPointerRow;
using skyroads::data::TrekdatRecord;
using skyroads::data::TrekdatShape;

namespace {

constexpr std::size_t FRAMEBUFFER_WIDTH = 320;
constexpr std::size_t FRAMEBUFFER_HEIGHT = 200;
constexpr std::size_t DASHBOARD_TOP = 138;
constexpr std::size_t HORIZON_Y = 24;
constexpr std::size_t VIEW_BOTTOM_Y = DASHBOARD_TOP;
constexpr std::size_t SHIP_SCALE = 1;
constexpr int32_t SHIP_SCREEN_X = 160;
constexpr int32_t SHIP_SCREEN_Y = 84;
constexpr int32_t DEBUG_PANEL_X = 8;
constexpr int32_t DEBUG_PANEL_Y = 8;
constexpr int32_t DEBUG_PANEL_W = 124;
constexpr int32_t DEBUG_PANEL_H = 42;
constexpr int32_t DEBUG_TOPDOWN_INSET_X = 206;
constexpr int32_t DEBUG_TOPDOWN_INSET_Y = 28;
constexpr int32_t DEBUG_TOPDOWN_INSET_W = 104;
constexpr int32_t DEBUG_TOPDOWN_INSET_H = 84;

constexpr std::size_t DOS_LEFT_CELL_COLUMNS[4] = {0, 1, 2, 3};
constexpr std::size_t DOS_RIGHT_CELL_COLUMNS[4] = {6, 5, 4, 3};

std::size_t sat_sub(std::size_t a, std::size_t b) { return a > b ? a - b : 0; }
uint8_t sat_add_u8(uint8_t a, uint8_t b) {
    const unsigned s = static_cast<unsigned>(a) + b;
    return s > 255 ? 255 : static_cast<uint8_t>(s);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

RgbColor scale_brightness(RgbColor color, float brightness) {
    brightness = std::clamp(brightness, 0.0f, 1.35f);
    auto ch = [&](uint8_t v) -> uint8_t {
        const float scaled = std::clamp(std::round(static_cast<float>(v) * brightness),
                                        0.0f, 255.0f);
        return static_cast<uint8_t>(scaled);
    };
    return RgbColor(ch(color.r), ch(color.g), ch(color.b));
}

RgbColor road_color(LevelCell cell) {
    switch (cell.tile_effect) {
        case TouchEffect::Accelerate: return RgbColor(126, 184, 118);
        case TouchEffect::Decelerate: return RgbColor(183, 146, 106);
        case TouchEffect::Kill: return RgbColor(214, 59, 92);
        case TouchEffect::Slide: return RgbColor(103, 132, 206);
        case TouchEffect::RefillOxygen: return RgbColor(92, 183, 202);
        case TouchEffect::None:
            if (cell.cube_height.has_value()) return RgbColor(222, 72, 112);
            if (cell.has_tunnel) return RgbColor(156, 128, 94);
            return RgbColor(172, 173, 194);
    }
    return RgbColor(172, 173, 194);
}

RgbColor road_edge_color(LevelCell cell) {
    if (cell.cube_height.has_value()) return RgbColor(245, 109, 136);
    return scale_brightness(road_color(cell), 1.2f);
}

// ---- internal geometry structs (referenced by ReferenceRenderer methods) ---

struct RoadSpan {
    std::size_t start_column;
    std::size_t end_column_exclusive;
    LevelCell sample_cell;
};

struct ProjectedRoadSpan {
    float top_start;
    float top_end;
    float bottom_start;
    float bottom_end;
    LevelCell sample_cell;
};

struct ProjectedObstacle {
    float column_start;
    float column_end;
    float height_factor;
    RgbColor color;
};

enum class DosRenderSide { Left, Right };

struct TrekdatProjectionKey {
    std::size_t depth_index;
    std::size_t road_row_group;
    std::size_t trekdat_slot;
};

struct RoadCellBytes {
    uint8_t byte0;
    uint8_t byte1;
    static RoadCellBytes from_cell(LevelCell cell) {
        return {static_cast<uint8_t>(cell.raw_descriptor),
                static_cast<uint8_t>(cell.raw_descriptor >> 8)};
    }
    uint8_t low_nibble() const { return byte0 & 0x0F; }
    uint8_t high_nibble() const { return byte0 >> 4; }
    std::size_t dispatch_kind() const { return byte1 & 0x0F; }
};

enum class DosRoadPhase { BeforeShip, AfterShip };

struct DosCellContext {
    LevelCell current_cell;
    RoadCellBytes current;
    RoadCellBytes inward;
    RoadCellBytes nearer;
};

struct PrimitiveCursor {
    std::optional<uint16_t> next_offset;
    explicit PrimitiveCursor(uint16_t start) : next_offset(start) {}

    bool skip(const TrekdatRecord& record) {
        if (!next_offset) return false;
        next_offset = record.next_shape_offset(*next_offset);
        return true;
    }

    bool emit(FrameBuffer320x200& frame, const TrekdatRecord& record,
              LevelCell cell, DosRenderSide side,
              std::optional<uint8_t> override_color_code);
};

uint8_t nonzero_or(uint8_t value, uint8_t fallback) {
    return value == 0 ? fallback : value;
}

// Current road's VGA palette for the frame being drawn (set in
// render_play_scene). The DOS road renderer colours each TREKDAT span by
// indexing this palette with the shape's colour code, which yields the exact
// road greys and wall pinks. Single-threaded renderer, so a file-local is safe.
const std::vector<RgbColor>* g_road_palette = nullptr;

RgbColor dos_shape_color(LevelCell cell, uint8_t color_code) {
    if (g_road_palette != nullptr && color_code < g_road_palette->size()) {
        return (*g_road_palette)[color_code];
    }
    // Fallback (no palette available): approximate from the tile's base colour.
    const RgbColor base = road_color(cell);
    if (color_code >= 0x01 && color_code <= 0x0E) {
        return scale_brightness(base, 0.98f + static_cast<float>(color_code & 0x0F) * 0.02f);
    }
    if (color_code >= 0x0F && color_code <= 0x1D) return scale_brightness(base, 1.18f);
    if (color_code >= 0x1E && color_code <= 0x2D) return scale_brightness(base, 0.78f);
    return scale_brightness(base, 0.92f);
}

void draw_trekdat_span(FrameBuffer320x200& frame, int32_t x, int32_t y,
                       int32_t width, RgbColor color) {
    if (y < static_cast<int32_t>(HORIZON_Y) || y >= static_cast<int32_t>(VIEW_BOTTOM_Y)) {
        return;
    }
    frame.fill_rect(x, y, width, 1, color);
}

void draw_dos_shape(FrameBuffer320x200& frame, const TrekdatShape& shape,
                    RgbColor color, DosRenderSide side) {
    for (const auto& span : shape.spans) {
        if (span.width == 0) continue;
        int32_t x;
        if (side == DosRenderSide::Left) {
            x = static_cast<int32_t>(span.x);
        } else {
            x = static_cast<int32_t>(SCREEN_WIDTH) - static_cast<int32_t>(span.x) -
                static_cast<int32_t>(span.width);
        }
        draw_trekdat_span(frame, x, static_cast<int32_t>(span.y),
                          static_cast<int32_t>(span.width), color);
    }
}

using LevelRow7 = std::array<LevelCell, ROAD_COLUMNS>;

LevelRow7 empty_row() {
    LevelRow7 row;
    for (auto& c : row) c = LevelCell::empty();
    return row;
}

LevelRow7 scene_row(const DemoPlaybackState& scene, int64_t row_index) {
    if (row_index < 0) return empty_row();
    for (const auto& row : scene.rows) {
        if (row.row_index == static_cast<std::size_t>(row_index)) return row.cells;
    }
    return empty_row();
}

std::vector<RoadSpan> road_surface_spans(const LevelRow7& row) {
    std::vector<RoadSpan> spans;
    std::optional<std::pair<std::size_t, LevelCell>> start;
    for (std::size_t index = 0; index < row.size(); ++index) {
        const LevelCell cell = row[index];
        const bool is_surface = cell.has_tile;
        if (!start && is_surface) {
            start = std::make_pair(index, cell);
        } else if (start && !is_surface) {
            spans.push_back(RoadSpan{start->first, index, start->second});
            start.reset();
        }
    }
    if (start) {
        spans.push_back(RoadSpan{start->first, ROAD_COLUMNS, start->second});
    }
    return spans;
}

// ---- ship visual helpers ---------------------------------------------------

int32_t dos_ship_lane_index(double x_position) {
    const int32_t coarse_x = static_cast<int32_t>(std::floor(x_position));
    return std::clamp((coarse_x - 95) / 46, 0, 6);
}

int32_t dos_ship_vertical_state(const DemoPlaybackState& scene) {
    const double SHIP_RISE_THRESHOLD = 0x163 / 128.0;
    if (scene.ship.y_position < GROUND_Y || scene.ship.z_position < 0.0) return 2;
    if (scene.ship.y_velocity <= -SHIP_RISE_THRESHOLD) return 2;
    if (scene.ship.y_velocity >= SHIP_RISE_THRESHOLD) return 1;
    return 0;
}

} // namespace

// ProjectedRoadSlice is referenced in the header method signatures, so it lives
// in the namespace (not anonymous).
struct ProjectedRoadSlice {
    TrekdatProjectionKey trekdat_key;
    std::size_t top_y;
    std::size_t bottom_y;
    float center_top;
    float center_bottom;
    float width_top;
    float width_bottom;
    std::vector<ProjectedRoadSpan> spans;
    std::vector<ProjectedObstacle> obstacles;
    std::optional<std::pair<float, float>> tunnel_span;
};

namespace {

double road_depth_current_z(const DemoPlaybackState& scene) {
    return static_cast<double>(scene.current_row) / 8.0 + scene.fractional_z;
}

float road_depth(const DemoPlaybackState& scene, std::size_t row_index) {
    const double current_z = road_depth_current_z(scene);
    return static_cast<float>(
        std::max((static_cast<double>(row_index) + 1.0) - current_z, 0.0));
}

std::size_t projected_y_for_depth(float depth, float far_depth) {
    const float view_height = static_cast<float>(VIEW_BOTTOM_Y - HORIZON_Y);
    const float near_plane = 0.45f;
    const float inverse = 1.0f / (depth + near_plane);
    const float inverse_near = 1.0f / near_plane;
    const float inverse_far = 1.0f / (far_depth + near_plane);
    const float normalized =
        std::clamp((inverse - inverse_far) / (inverse_near - inverse_far), 0.0f, 1.0f);
    return static_cast<std::size_t>(
        std::round(static_cast<float>(HORIZON_Y) + view_height * normalized));
}

float projected_width_for_depth(float depth, float far_depth) {
    const float near_width = 252.0f;
    const float far_width = 34.0f;
    const float near_plane = 0.45f;
    const float inverse = 1.0f / (depth + near_plane);
    const float inverse_near = 1.0f / near_plane;
    const float inverse_far = 1.0f / (far_depth + near_plane);
    const float normalized =
        std::clamp((inverse - inverse_far) / (inverse_near - inverse_far), 0.0f, 1.0f);
    return lerp(far_width, near_width, std::pow(normalized, 0.75f));
}

float projected_center_x(const DemoPlaybackState& scene, float depth, float far_depth);

std::vector<ProjectedRoadSpan> project_surface_spans(
    const std::vector<RoadSpan>& top_spans,
    const std::vector<RoadSpan>& bottom_spans) {
    const std::size_t count = std::max(top_spans.size(), bottom_spans.size());
    std::vector<ProjectedRoadSpan> spans;
    for (std::size_t index = 0; index < count; ++index) {
        const RoadSpan* top_span =
            index < top_spans.size() ? &top_spans[index]
                                     : (top_spans.empty() ? nullptr : &top_spans.back());
        const RoadSpan* bottom_span =
            index < bottom_spans.size()
                ? &bottom_spans[index]
                : (bottom_spans.empty() ? nullptr : &bottom_spans.back());
        std::optional<LevelCell> sample_cell;
        if (bottom_span) sample_cell = bottom_span->sample_cell;
        else if (top_span) sample_cell = top_span->sample_cell;
        if (!sample_cell) continue;
        const RoadSpan* t = top_span ? top_span : bottom_span;
        const RoadSpan* b = bottom_span ? bottom_span : t;
        spans.push_back(ProjectedRoadSpan{
            static_cast<float>(t->start_column) / static_cast<float>(ROAD_COLUMNS),
            static_cast<float>(t->end_column_exclusive) / static_cast<float>(ROAD_COLUMNS),
            static_cast<float>(b->start_column) / static_cast<float>(ROAD_COLUMNS),
            static_cast<float>(b->end_column_exclusive) / static_cast<float>(ROAD_COLUMNS),
            *sample_cell});
    }
    return spans;
}

std::vector<ProjectedObstacle> project_obstacles(const RoadRenderRow& row,
                                                 float near_depth, float far_depth) {
    const float visibility =
        std::clamp(1.0f - (near_depth / std::max(far_depth, 1.0f)), 0.0f, 1.0f);
    std::vector<ProjectedObstacle> obstacles;
    for (std::size_t index = 0; index < row.cells.size(); ++index) {
        const LevelCell& cell = row.cells[index];
        if (!cell.cube_height.has_value()) continue;
        obstacles.push_back(ProjectedObstacle{
            static_cast<float>(index) / static_cast<float>(ROAD_COLUMNS),
            static_cast<float>(index + 1) / static_cast<float>(ROAD_COLUMNS),
            (*cell.cube_height >= 120 ? 0.8f : 0.55f) * std::max(visibility, 0.2f),
            cell.has_tunnel ? RgbColor(198, 74, 112) : RgbColor(230, 64, 94)});
    }
    return obstacles;
}

std::optional<std::pair<float, float>> project_tunnel_span(const RoadRenderRow& row) {
    std::size_t min_column = ROAD_COLUMNS;
    std::size_t max_column = 0;
    bool found = false;
    for (std::size_t index = 0; index < row.cells.size(); ++index) {
        const LevelCell& cell = row.cells[index];
        if (cell.has_tunnel && cell.has_tile) {
            min_column = std::min(min_column, index);
            max_column = std::max(max_column, index + 1);
            found = true;
        }
    }
    if (!found) return std::nullopt;
    return std::make_pair(
        static_cast<float>(min_column) / static_cast<float>(ROAD_COLUMNS),
        static_cast<float>(max_column) / static_cast<float>(ROAD_COLUMNS));
}

int32_t project_span_x(float center, float road_width, float top_value,
                       float bottom_value, float t) {
    const float edge_fraction = lerp(top_value, bottom_value, t);
    return static_cast<int32_t>(
        std::round(center - road_width / 2.0f + road_width * edge_fraction));
}

std::vector<ProjectedRoadSlice> project_road_slices(const DemoPlaybackState& scene) {
    if (scene.rows.size() < 2) return {};

    float far_depth = 20.0f;
    if (!scene.rows.empty()) {
        far_depth = road_depth(scene, scene.rows.back().row_index) + 1.0f;
    }
    far_depth = std::max(far_depth, 12.0f);

    std::vector<ProjectedRoadSlice> slices;
    for (std::size_t depth_index = 0; depth_index + 1 < scene.rows.size(); ++depth_index) {
        const RoadRenderRow& near_row = scene.rows[depth_index];
        const RoadRenderRow& far_row = scene.rows[depth_index + 1];
        const float near_depth = road_depth(scene, near_row.row_index);
        const float far_depth_for_row = road_depth(scene, far_row.row_index);
        if (far_depth_for_row <= near_depth) continue;

        const std::size_t top_y = projected_y_for_depth(far_depth_for_row, far_depth);
        const std::size_t bottom_y = projected_y_for_depth(near_depth, far_depth);
        if (bottom_y <= top_y || top_y >= VIEW_BOTTOM_Y) continue;

        const float width_top = projected_width_for_depth(far_depth_for_row, far_depth);
        const float width_bottom = projected_width_for_depth(near_depth, far_depth);
        const float center_top = projected_center_x(scene, far_depth_for_row, far_depth);
        const float center_bottom = projected_center_x(scene, near_depth, far_depth);
        const std::vector<RoadSpan> top_spans = road_surface_spans(far_row.cells);
        const std::vector<RoadSpan> bottom_spans = road_surface_spans(near_row.cells);
        const auto row_state = renderer_row_state(static_cast<uint16_t>(near_row.row_index));

        slices.push_back(ProjectedRoadSlice{
            TrekdatProjectionKey{depth_index, row_state.road_row_group,
                                 row_state.trekdat_slot},
            top_y,
            std::min(bottom_y, VIEW_BOTTOM_Y),
            center_top,
            center_bottom,
            width_top,
            width_bottom,
            project_surface_spans(top_spans, bottom_spans),
            project_obstacles(near_row, near_depth, far_depth),
            project_tunnel_span(near_row)});
    }

    std::reverse(slices.begin(), slices.end());
    return slices;
}

} // namespace

DerivedShipVisualState derive_ship_visual_state(const DemoPlaybackState& scene) {
    const int32_t lane_index = dos_ship_lane_index(scene.ship.x_position);
    ShipBank bank;
    if (lane_index < 3) bank = ShipBank::Left;
    else if (lane_index == 3) bank = ShipBank::Center;
    else bank = ShipBank::Right;

    // Death animations (EXE @0xc02, re/NOTES.md Update 9): only a CRASH runs the
    // explosion counter ds:0x4578. Falling off the road / running out of fuel or
    // oxygen leave it at 0, so the normal flight pose keeps being drawn while the
    // ship falls away — there is no explosion for those.
    ShipSpriteKind sprite_kind;
    switch (scene.ship.state) {
        case ShipState::Exploded: sprite_kind = ShipSpriteKind::Exploding; break;
        case ShipState::Alive:
        default: sprite_kind = ShipSpriteKind::Alive; break;
    }

    const bool jumping = scene.ship.y_position > GROUND_Y + 0.5 ||
                         scene.ship.is_going_up || !scene.ship.is_on_ground ||
                         scene.ship.jump_input;
    const int32_t vertical_state = dos_ship_vertical_state(scene);
    const double lane_bias =
        (scene.ship.x_position - LEVEL_CENTER_X) / LEVEL_TILE_STRIDE_X;
    const int32_t ship_lane_bias = static_cast<int32_t>(std::round(lane_bias * 30.0));
    const int32_t ship_screen_bias_x = std::clamp(ship_lane_bias, -96, 96);
    const double height_delta = scene.ship.y_position - GROUND_Y;
    const int32_t vertical_offset_y = static_cast<int32_t>(
        std::clamp(std::round(-height_delta * 0.55), -26.0, 18.0));

    std::size_t explosion_frame = 0;
    if (scene.ship.death_frame_index.has_value()) {
        explosion_frame = sat_sub(scene.frame_index, *scene.ship.death_frame_index) / 3;
    }

    // Exact DOS pose formula (re/NOTES.md, wrapper @0xbe3/0xc93):
    //   sprite = (lane*3 + vstate)*3 + 14 + thrust ; our exact_ship_frames array
    //   is based at sprite 14, so the array index drops the +14. thrust is the
    //   engine-flicker animation (added later); 0 gives the static pose.
    std::optional<std::size_t> exact_ship_frame_index;
    if (sprite_kind == ShipSpriteKind::Alive) {
        const int32_t v = std::clamp(vertical_state, 0, 2);
        // Thrust flicker: table ds:0xea = {0,1,2,1}, cycled by (frame/2)%4. With
        // no fuel the EXE forces the thrust value to 0 (engine flame off, @0xc26).
        static const int thrust_cycle[4] = {0, 1, 2, 1};
        const int thrust = scene.ship.state == ShipState::OutOfFuel
                               ? 0
                               : thrust_cycle[(scene.frame_index / 2) % 4];
        exact_ship_frame_index =
            static_cast<std::size_t>((lane_index * 3 + v) * 3 + thrust);
    }

    DerivedShipVisualState v;
    v.sprite_kind = sprite_kind;
    v.bank = bank;
    v.thrust_on = scene.ship.accel_input > 0 && scene.ship.state == ShipState::Alive;
    v.jumping = jumping;
    v.explosion_frame = explosion_frame;
    v.exact_ship_frame_index = exact_ship_frame_index;
    v.ship_screen_bias_x = ship_screen_bias_x;
    v.vertical_offset_y = vertical_offset_y;
    v.on_surface = scene.ship.is_on_ground && scene.ship.state == ShipState::Alive;
    v.casts_shadow = scene.ship.state == ShipState::Alive;
    return v;
}

namespace {

float projected_center_x(const DemoPlaybackState& scene, float depth, float far_depth) {
    const float ship_bias =
        static_cast<float>(derive_ship_visual_state(scene).ship_screen_bias_x);
    const float perspective =
        1.0f - (std::clamp(depth / std::max(far_depth, 1.0f), 0.0f, 1.0f) * 0.55f);
    return static_cast<float>(FRAMEBUFFER_WIDTH) / 2.0f - ship_bias * perspective;
}

// Exact DOS screen-X (re/NOTES.md, wrapper @0xe01 + blit @0x325c): the sprite's
// left edge = x_position - 110 + lane_adj, a 1:1 world->screen mapping (1 px per
// world unit, no compression, no clamp). Our sprite is 30 wide, so the centre is
// (x_position - 110 + adj) + 15 = x_position - 95 + adj. This makes the ship's
// visual position match the simulation exactly, so collisions line up and it can
// travel the full lane range. `slices` unused; kept for the shared signature.
ShipScreenPlacement ship_screen_placement_from_slices(
    const DemoPlaybackState& scene, const DerivedShipVisualState& visual,
    const std::vector<ProjectedRoadSlice>& /*slices*/) {
    static const int lane_adj[7] = {-1, -1, -1, 0, 1, 2, 4};
    const int lane = std::clamp(
        (static_cast<int>(std::floor(scene.ship.x_position)) - 95) / 46, 0, 6);
    const int32_t center_x =
        static_cast<int32_t>(std::lround(scene.ship.x_position)) - 95 + lane_adj[lane];
    const int32_t shadow_center_y = SHIP_SCREEN_Y + 18;
    // Sit the ship on the ground (its wheels/engines near the shadow) rather than
    // hovering above it. On the ground vertical_offset_y is 0; a jump lifts it.
    const int32_t sprite_center_y = SHIP_SCREEN_Y + 4 + visual.vertical_offset_y;
    return ShipScreenPlacement{center_x, sprite_center_y, center_x,
                               shadow_center_y};
}

ShipScreenPlacement ship_screen_placement(const DemoPlaybackState& scene,
                                          const DerivedShipVisualState& visual) {
    return ship_screen_placement_from_slices(scene, visual, {});
}

void stroke_rect(FrameBuffer320x200& frame, int32_t x, int32_t y, int32_t w,
                 int32_t h, RgbColor color) {
    if (w <= 0 || h <= 0) return;
    frame.fill_rect(x, y, w, 1, color);
    frame.fill_rect(x, y + h - 1, w, 1, color);
    frame.fill_rect(x, y, 1, h, color);
    frame.fill_rect(x + w - 1, y, 1, h, color);
}

RgbColor debug_cell_color(LevelCell cell) {
    if (cell.is_empty()) return RgbColor(20, 22, 28);
    if (cell.cube_height.has_value() && cell.has_tunnel) return RgbColor(203, 112, 82);
    if (cell.cube_height.has_value()) return RgbColor(210, 76, 110);
    if (cell.has_tunnel) return RgbColor(153, 119, 82);
    if (cell.has_tile) return road_color(cell);
    return RgbColor(52, 58, 72);
}

const char* short_ship_state(ShipState state) {
    switch (state) {
        case ShipState::Alive: return "ALIVE";
        case ShipState::Exploded: return "EXPLODED";
        case ShipState::Fallen: return "FALLEN";
        case ShipState::OutOfFuel: return "NO FUEL";
        case ShipState::OutOfOxygen: return "NO OXY";
    }
    return "?";
}

int32_t text_pixel_width(const std::string& text, std::size_t scale) {
    const int32_t glyph_width = static_cast<int32_t>(4 * scale);
    const int32_t total = static_cast<int32_t>(text.size()) * glyph_width;
    return total > static_cast<int32_t>(scale) ? total - static_cast<int32_t>(scale) : 0;
}

std::optional<std::array<uint8_t, 5>> glyph_rows(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    switch (ch) {
        case 'A': return std::array<uint8_t, 5>{0b010, 0b101, 0b111, 0b101, 0b101};
        case 'B': return std::array<uint8_t, 5>{0b110, 0b101, 0b110, 0b101, 0b110};
        case 'C': return std::array<uint8_t, 5>{0b011, 0b100, 0b100, 0b100, 0b011};
        case 'D': return std::array<uint8_t, 5>{0b110, 0b101, 0b101, 0b101, 0b110};
        case 'E': return std::array<uint8_t, 5>{0b111, 0b100, 0b110, 0b100, 0b111};
        case 'F': return std::array<uint8_t, 5>{0b111, 0b100, 0b110, 0b100, 0b100};
        case 'G': return std::array<uint8_t, 5>{0b011, 0b100, 0b101, 0b101, 0b011};
        case 'H': return std::array<uint8_t, 5>{0b101, 0b101, 0b111, 0b101, 0b101};
        case 'I': return std::array<uint8_t, 5>{0b111, 0b010, 0b010, 0b010, 0b111};
        case 'J': return std::array<uint8_t, 5>{0b001, 0b001, 0b001, 0b101, 0b010};
        case 'K': return std::array<uint8_t, 5>{0b101, 0b101, 0b110, 0b101, 0b101};
        case 'L': return std::array<uint8_t, 5>{0b100, 0b100, 0b100, 0b100, 0b111};
        case 'M': return std::array<uint8_t, 5>{0b101, 0b111, 0b111, 0b101, 0b101};
        case 'N': return std::array<uint8_t, 5>{0b101, 0b111, 0b111, 0b111, 0b101};
        case 'O': return std::array<uint8_t, 5>{0b010, 0b101, 0b101, 0b101, 0b010};
        case 'P': return std::array<uint8_t, 5>{0b110, 0b101, 0b110, 0b100, 0b100};
        case 'Q': return std::array<uint8_t, 5>{0b010, 0b101, 0b101, 0b011, 0b001};
        case 'R': return std::array<uint8_t, 5>{0b110, 0b101, 0b110, 0b101, 0b101};
        case 'S': return std::array<uint8_t, 5>{0b011, 0b100, 0b010, 0b001, 0b110};
        case 'T': return std::array<uint8_t, 5>{0b111, 0b010, 0b010, 0b010, 0b010};
        case 'U': return std::array<uint8_t, 5>{0b101, 0b101, 0b101, 0b101, 0b111};
        case 'V': return std::array<uint8_t, 5>{0b101, 0b101, 0b101, 0b101, 0b010};
        case 'W': return std::array<uint8_t, 5>{0b101, 0b101, 0b111, 0b111, 0b101};
        case 'X': return std::array<uint8_t, 5>{0b101, 0b101, 0b010, 0b101, 0b101};
        case 'Y': return std::array<uint8_t, 5>{0b101, 0b101, 0b010, 0b010, 0b010};
        case 'Z': return std::array<uint8_t, 5>{0b111, 0b001, 0b010, 0b100, 0b111};
        case '0': return std::array<uint8_t, 5>{0b111, 0b101, 0b101, 0b101, 0b111};
        case '1': return std::array<uint8_t, 5>{0b010, 0b110, 0b010, 0b010, 0b111};
        case '2': return std::array<uint8_t, 5>{0b110, 0b001, 0b111, 0b100, 0b111};
        case '3': return std::array<uint8_t, 5>{0b110, 0b001, 0b111, 0b001, 0b110};
        case '4': return std::array<uint8_t, 5>{0b101, 0b101, 0b111, 0b001, 0b001};
        case '5': return std::array<uint8_t, 5>{0b111, 0b100, 0b111, 0b001, 0b110};
        case '6': return std::array<uint8_t, 5>{0b011, 0b100, 0b111, 0b101, 0b111};
        case '7': return std::array<uint8_t, 5>{0b111, 0b001, 0b010, 0b010, 0b010};
        case '8': return std::array<uint8_t, 5>{0b111, 0b101, 0b111, 0b101, 0b111};
        case '9': return std::array<uint8_t, 5>{0b111, 0b101, 0b111, 0b001, 0b110};
        case '.': return std::array<uint8_t, 5>{0b000, 0b000, 0b000, 0b000, 0b010};
        default: return std::nullopt;
    }
}

// ---- DOS TREKDAT road pass -------------------------------------------------

void draw_dos_type_0(FrameBuffer320x200& frame, const TrekdatRecord& record,
                     const TrekdatCellPointers& cell_pointers,
                     const DosCellContext& context, DosRenderSide side) {
    const uint8_t base_color = context.current.low_nibble();
    if (base_color == 0) return;

    PrimitiveCursor cursor(cell_pointers.pointers[0]);
    cursor.emit(frame, record, context.current_cell, side, base_color);
    if (context.inward.low_nibble() == 0) {
        cursor.emit(frame, record, context.current_cell, side,
                    sat_add_u8(base_color, 0x1E));
    } else {
        cursor.skip(record);
    }
    if (context.nearer.low_nibble() == 0) {
        cursor.emit(frame, record, context.current_cell, side,
                    sat_add_u8(base_color, 0x0F));
    }
}

void draw_dos_type_1(FrameBuffer320x200& frame, const TrekdatRecord& record,
                     const TrekdatCellPointers& cell_pointers,
                     const DosCellContext& context, DosRenderSide side) {
    draw_dos_type_0(frame, record, cell_pointers, context, side);
    if (context.nearer.byte1 < 1) {
        PrimitiveCursor cursor(cell_pointers.pointers[1]);
        cursor.emit(frame, record, context.current_cell, side, uint8_t{0x43});
    }
    PrimitiveCursor cursor(cell_pointers.pointers[4]);
    for (int i = 0; i < 6; ++i) {
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
    if (context.nearer.byte1 < 1) {
        for (int i = 0; i < 2; ++i) {
            cursor.emit(frame, record, context.current_cell, side, std::nullopt);
        }
    }
}

void draw_dos_type_2(FrameBuffer320x200& frame, const TrekdatRecord& record,
                     const TrekdatCellPointers& cell_pointers,
                     const DosCellContext& context, DosRenderSide side) {
    draw_dos_type_0(frame, record, cell_pointers, context, side);
    if (context.nearer.byte1 < 2) {
        PrimitiveCursor cursor(cell_pointers.pointers[3]);
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
    const uint8_t override_color = nonzero_or(context.current.high_nibble(), 0x3D);
    PrimitiveCursor cursor(cell_pointers.pointers[2]);
    cursor.emit(frame, record, context.current_cell, side, override_color);
    if (context.inward.byte1 < 2) {
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
}

void draw_dos_type_3(FrameBuffer320x200& frame, const TrekdatRecord& record,
                     const TrekdatCellPointers& cell_pointers,
                     const DosCellContext& context, DosRenderSide side) {
    draw_dos_type_0(frame, record, cell_pointers, context, side);
    if (context.nearer.byte1 < 2) {
        PrimitiveCursor cursor(cell_pointers.pointers[1]);
        cursor.emit(frame, record, context.current_cell, side, uint8_t{0x41});
    }
    const uint8_t override_color = nonzero_or(context.current.high_nibble(), 0x3D);
    {
        PrimitiveCursor cursor(cell_pointers.pointers[2]);
        cursor.emit(frame, record, context.current_cell, side, override_color);
        if (context.inward.byte1 < 2) {
            cursor.emit(frame, record, context.current_cell, side, std::nullopt);
        }
    }
    if (context.nearer.byte1 < 2) {
        PrimitiveCursor cursor(cell_pointers.pointers[3]);
        cursor.skip(record);
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
}

void draw_dos_type_4(FrameBuffer320x200& frame, const TrekdatRecord& record,
                     const TrekdatCellPointers& cell_pointers,
                     const DosCellContext& context, DosRenderSide side) {
    draw_dos_type_0(frame, record, cell_pointers, context, side);
    if (context.nearer.byte1 < 2) {
        PrimitiveCursor cursor(cell_pointers.pointers[3]);
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
    {
        PrimitiveCursor cursor(cell_pointers.pointers[2]);
        cursor.skip(record);
        if (context.inward.byte1 < 2) {
            cursor.emit(frame, record, context.current_cell, side, std::nullopt);
        }
    }
    const uint8_t override_color = nonzero_or(context.current.high_nibble(), 0x3D);
    PrimitiveCursor cursor(cell_pointers.pointers[5]);
    cursor.emit(frame, record, context.current_cell, side, override_color);
    if (context.inward.byte1 < 4) {
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    } else {
        cursor.skip(record);
    }
    if (context.nearer.byte1 < 4) {
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
}

void draw_dos_type_5(FrameBuffer320x200& frame, const TrekdatRecord& record,
                     const TrekdatCellPointers& cell_pointers,
                     const DosCellContext& context, DosRenderSide side) {
    draw_dos_type_0(frame, record, cell_pointers, context, side);
    if (context.nearer.byte1 < 2) {
        PrimitiveCursor cursor(cell_pointers.pointers[1]);
        cursor.emit(frame, record, context.current_cell, side, uint8_t{0x41});
    }
    {
        PrimitiveCursor cursor(cell_pointers.pointers[2]);
        cursor.skip(record);
        if (context.inward.byte1 < 2) {
            cursor.emit(frame, record, context.current_cell, side, std::nullopt);
        }
    }
    if (context.nearer.byte1 < 2) {
        PrimitiveCursor cursor(cell_pointers.pointers[3]);
        cursor.skip(record);
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
    const uint8_t override_color = nonzero_or(context.current.high_nibble(), 0x3D);
    PrimitiveCursor cursor(cell_pointers.pointers[5]);
    cursor.emit(frame, record, context.current_cell, side, override_color);
    if (context.inward.byte1 < 4) {
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    } else {
        cursor.skip(record);
    }
    if (context.nearer.byte1 < 4) {
        cursor.emit(frame, record, context.current_cell, side, std::nullopt);
    }
}

void draw_dos_cell(FrameBuffer320x200& frame, const TrekdatRecord& record,
                   const TrekdatCellPointers& cell_pointers,
                   const DosCellContext& context, DosRenderSide side) {
    switch (context.current.dispatch_kind()) {
        case 0: draw_dos_type_0(frame, record, cell_pointers, context, side); break;
        case 1: draw_dos_type_1(frame, record, cell_pointers, context, side); break;
        case 2: draw_dos_type_2(frame, record, cell_pointers, context, side); break;
        case 3: draw_dos_type_3(frame, record, cell_pointers, context, side); break;
        case 4: draw_dos_type_4(frame, record, cell_pointers, context, side); break;
        case 5: draw_dos_type_5(frame, record, cell_pointers, context, side); break;
        default: break;
    }
}

void draw_dos_pointer_row(FrameBuffer320x200& frame, const TrekdatRecord& record,
                          const TrekdatPointerRow& pointer_row,
                          const LevelRow7& current_row, const LevelRow7& nearer_row) {
    struct SideCols {
        DosRenderSide side;
        const std::size_t* columns;
    };
    const SideCols sides[2] = {{DosRenderSide::Left, DOS_LEFT_CELL_COLUMNS},
                               {DosRenderSide::Right, DOS_RIGHT_CELL_COLUMNS}};
    for (const auto& sc : sides) {
        for (std::size_t slot_index = 0; slot_index < 4; ++slot_index) {
            const std::size_t column_index = sc.columns[slot_index];
            std::size_t inward_index;
            if (sc.side == DosRenderSide::Left) {
                inward_index = std::min(column_index + 1, ROAD_COLUMNS - 1);
            } else {
                inward_index = sat_sub(column_index, 1);
            }
            DosCellContext context{
                current_row[column_index],
                RoadCellBytes::from_cell(current_row[column_index]),
                RoadCellBytes::from_cell(current_row[inward_index]),
                RoadCellBytes::from_cell(nearer_row[column_index])};
            draw_dos_cell(frame, record, pointer_row.cells[slot_index], context,
                          sc.side);
        }
    }
}

bool draw_dos_trekdat_pass(FrameBuffer320x200& frame, const DemoPlaybackState& scene,
                           const TrekdatRecord& record, DosRoadPhase phase) {
    if (scene.rows.empty()) return false;

    const auto pointer_rows = record.dos_pointer_layout();
    const int64_t current_group = static_cast<int64_t>(scene.current_row >> 3);

    struct RowStep {
        std::size_t pointer_row_index;
        int64_t row_offset;
    };
    static const RowStep before_seq[] = {{0, 7}, {1, 6}, {2, 5}, {3, 4},
                                         {4, 3}, {5, 2}, {6, 1}, {11, 0}};
    static const RowStep after_seq[] = {{12, 0}, {8, -1}, {9, -2}, {10, -3}};

    const RowStep* seq = phase == DosRoadPhase::BeforeShip ? before_seq : after_seq;
    const std::size_t seq_len = phase == DosRoadPhase::BeforeShip ? 8 : 4;

    for (std::size_t i = 0; i < seq_len; ++i) {
        const int64_t row_index = current_group + seq[i].row_offset;
        const LevelRow7 current = scene_row(scene, row_index);
        const LevelRow7 nearer = scene_row(scene, row_index - 1);
        draw_dos_pointer_row(frame, record, pointer_rows.rows[seq[i].pointer_row_index],
                             current, nearer);
    }
    return true;
}

// ---- sprite atlas helpers --------------------------------------------------

// The DOS blit reads the 24-wide sprite row-major and writes each row down a
// screen column, i.e. sprite(row r, col c) -> screen(x = r, y = c). That is a
// transpose (a 90deg rotation plus a horizontal flip), so the on-screen sprite
// is `height` wide by `width` tall. Using a plain rotation instead mirrors the
// ship and reverses its bank direction.
ImageFrame transpose_sprite(const ImageFrame& sprite) {
    const std::size_t width = sprite.width;   // 24 (columns)
    const std::size_t height = sprite.height; // 30 (rows)
    ImageFrame out;
    out.offset = sprite.offset;
    out.width = static_cast<uint16_t>(height);
    out.height = static_cast<uint16_t>(width);
    out.palette = sprite.palette;
    out.transparent_zero = sprite.transparent_zero;
    out.pixels.assign(width * height, 0);
    for (std::size_t r = 0; r < height; ++r) {
        for (std::size_t c = 0; c < width; ++c) {
            out.pixels[c * height + r] = sprite.pixels[r * width + c];
        }
    }
    return out;
}

} // namespace

// ---- PrimitiveCursor::emit (needs draw_dos_shape/dos_shape_color) ----------

namespace {
bool PrimitiveCursor::emit(FrameBuffer320x200& frame, const TrekdatRecord& record,
                           LevelCell cell, DosRenderSide side,
                           std::optional<uint8_t> override_color_code) {
    if (!next_offset) return false;
    const uint16_t offset = *next_offset;
    auto shape = record.shape_at_offset(offset);
    if (!shape) {
        next_offset = std::nullopt;
        return false;
    }
    next_offset = record.next_shape_offset(offset);
    if (shape->span_count == 0) return true;
    const uint8_t color_code = override_color_code ? *override_color_code : shape->color;
    const RgbColor color = dos_shape_color(cell, color_code);
    draw_dos_shape(frame, *shape, color, side);
    return true;
}
} // namespace

// ---- FrameBuffer -----------------------------------------------------------

FrameBuffer320x200::FrameBuffer320x200()
    : width(SCREEN_WIDTH),
      height(SCREEN_HEIGHT),
      pixels_rgba(FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT * 4, 0) {}

void FrameBuffer320x200::clear(RgbColor color) {
    for (std::size_t i = 0; i < pixels_rgba.size(); i += 4) {
        pixels_rgba[i] = color.r;
        pixels_rgba[i + 1] = color.g;
        pixels_rgba[i + 2] = color.b;
        pixels_rgba[i + 3] = 255;
    }
}

void FrameBuffer320x200::fill_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                                   RgbColor color) {
    const std::size_t x0 = static_cast<std::size_t>(std::max(x, 0));
    const std::size_t y0 = static_cast<std::size_t>(std::max(y, 0));
    const std::size_t x1 = static_cast<std::size_t>(
        std::max(std::min(x + w, static_cast<int32_t>(FRAMEBUFFER_WIDTH)), 0));
    const std::size_t y1 = static_cast<std::size_t>(
        std::max(std::min(y + h, static_cast<int32_t>(FRAMEBUFFER_HEIGHT)), 0));
    for (std::size_t yy = y0; yy < y1; ++yy) {
        for (std::size_t xx = x0; xx < x1; ++xx) {
            set_pixel(xx, yy, color);
        }
    }
}

void FrameBuffer320x200::set_pixel(std::size_t x, std::size_t y, RgbColor color) {
    if (x >= FRAMEBUFFER_WIDTH || y >= FRAMEBUFFER_HEIGHT) return;
    const std::size_t offset = (y * FRAMEBUFFER_WIDTH + x) * 4;
    pixels_rgba[offset] = color.r;
    pixels_rgba[offset + 1] = color.g;
    pixels_rgba[offset + 2] = color.b;
    pixels_rgba[offset + 3] = 255;
}

void FrameBuffer320x200::blend_pixel(std::size_t x, std::size_t y, RgbColor color,
                                     float alpha) {
    if (x >= FRAMEBUFFER_WIDTH || y >= FRAMEBUFFER_HEIGHT) return;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const std::size_t offset = (y * FRAMEBUFFER_WIDTH + x) * 4;
    const float dr = pixels_rgba[offset];
    const float dg = pixels_rgba[offset + 1];
    const float db = pixels_rgba[offset + 2];
    pixels_rgba[offset] = static_cast<uint8_t>(
        std::round(dr * (1.0f - alpha) + static_cast<float>(color.r) * alpha));
    pixels_rgba[offset + 1] = static_cast<uint8_t>(
        std::round(dg * (1.0f - alpha) + static_cast<float>(color.g) * alpha));
    pixels_rgba[offset + 2] = static_cast<uint8_t>(
        std::round(db * (1.0f - alpha) + static_cast<float>(color.b) * alpha));
    pixels_rgba[offset + 3] = 255;
}

// ---- assets / atlas --------------------------------------------------------

AttractModeAssets AttractModeAssets::load_from_root(const std::string& source_root) {
    auto path = [&](const std::string& name) {
        if (!source_root.empty() && source_root.back() == '/') return source_root + name;
        return source_root + "/" + name;
    };
    AttractModeAssets a;
    for (int index = 0; index <= 9; ++index) {
        a.worlds.push_back(skyroads::data::load_image_archive_path(
            path("WORLD" + std::to_string(index) + ".LZS")));
    }
    a.intro = skyroads::data::load_image_archive_path(path("INTRO.LZS"));
    a.anim = skyroads::data::load_image_archive_path(path("ANIM.LZS"));
    a.main_menu = skyroads::data::load_image_archive_path(path("MAINMENU.LZS"));
    a.help_menu = skyroads::data::load_image_archive_path(path("HELPMENU.LZS"));
    a.settings_menu = skyroads::data::load_image_archive_path(path("SETMENU.LZS"));
    a.go_menu = skyroads::data::load_image_archive_path(path("GOMENU.LZS"));
    a.cars = skyroads::data::load_image_archive_path(path("CARS.LZS"));
    a.dashboard = skyroads::data::load_image_archive_path(path("DASHBRD.LZS"));
    a.trekdat = skyroads::data::load_trekdat_lzs_path(path("TREKDAT.LZS"));
    a.oxygen_gauge = skyroads::data::load_dashboard_dat_path(path("OXY_DISP.DAT"));
    a.fuel_gauge = skyroads::data::load_dashboard_dat_path(path("FUL_DISP.DAT"));
    a.speed_gauge = skyroads::data::load_dashboard_dat_path(path("SPEED.DAT"));
    return a;
}

namespace {
// The DOS renderer treats CARS as a flat array of fixed 24x30 (720-byte) sprites
// indexed by pose number (block n = rows [n*30, n*30+30)). The old code split on
// blank rows, which produced a different sprite count/boundaries and misaligned
// every index. Extract fixed blocks to match the executable exactly.
constexpr std::size_t CARS_W = 24;
constexpr std::size_t CARS_H = 30;

ImageFrame cars_block(const ImageFrame& sheet, std::size_t n) {
    ImageFrame out;
    out.width = static_cast<uint16_t>(CARS_W);
    out.height = static_cast<uint16_t>(CARS_H);
    out.palette = sheet.palette;
    out.transparent_zero = sheet.transparent_zero;
    out.pixels.resize(CARS_W * CARS_H, 0);
    const std::size_t base = n * CARS_H * CARS_W;
    for (std::size_t i = 0; i < CARS_W * CARS_H; ++i) {
        if (base + i < sheet.pixels.size()) out.pixels[i] = sheet.pixels[base + i];
    }
    return out;
}
} // namespace

std::optional<CarAtlas> CarAtlas::from_archive(const ImageArchive& archive) {
    if (archive.frames.empty() || archive.frames.front().empty()) return std::nullopt;
    const ImageFrame& frame = archive.frames.front().front();
    if (frame.width != CARS_W) return std::nullopt;
    const std::size_t block_count = frame.height / CARS_H; // 77 in the shipped set
    if (block_count < 77) return std::nullopt;

    CarAtlas atlas;
    // Sprites 0..13 are the explosion/destroyed set (death pose = timer/3).
    for (std::size_t i = 0; i < 14; ++i) {
        atlas.explosion_frames.push_back(transpose_sprite(cars_block(frame, i)));
    }
    // Sprites 14..76 are the flight poses: pose = (lane*3 + vstate)*3 + thrust,
    // with this array based at sprite 14.
    for (std::size_t i = 14; i < block_count; ++i) {
        atlas.exact_ship_frames.push_back(transpose_sprite(cars_block(frame, i)));
    }
    if (atlas.exact_ship_frames.empty()) return std::nullopt;
    atlas.destroyed = atlas.explosion_frames.back();
    return atlas;
}

const ImageFrame& CarAtlas::select_sprite(const DerivedShipVisualState& visual,
                                          std::size_t frame_index) const {
    switch (visual.sprite_kind) {
        case ShipSpriteKind::Exploding: {
            const std::size_t index =
                std::min(visual.explosion_frame, sat_sub(explosion_frames.size(), 1));
            return explosion_frames[index];
        }
        case ShipSpriteKind::Destroyed:
            return destroyed;
        case ShipSpriteKind::Alive: {
            (void)frame_index;
            std::size_t index =
                visual.exact_ship_frame_index ? *visual.exact_ship_frame_index : 0;
            index = std::min(index, sat_sub(exact_ship_frames.size(), 1));
            return exact_ship_frames[index];
        }
    }
    return destroyed;
}

// ---- DebugViewMode ---------------------------------------------------------

DebugViewMode debug_next(DebugViewMode mode) {
    switch (mode) {
        case DebugViewMode::Off: return DebugViewMode::Overlay;
        case DebugViewMode::Overlay: return DebugViewMode::Geometry;
        case DebugViewMode::Geometry: return DebugViewMode::TopDown;
        case DebugViewMode::TopDown: return DebugViewMode::Off;
    }
    return DebugViewMode::Off;
}

const char* debug_label(DebugViewMode mode) {
    switch (mode) {
        case DebugViewMode::Off: return "Normal";
        case DebugViewMode::Overlay: return "Overlay";
        case DebugViewMode::Geometry: return "Geometry";
        case DebugViewMode::TopDown: return "TopDown";
    }
    return "Normal";
}

// ---- ReferenceRenderer -----------------------------------------------------

ReferenceRenderer::ReferenceRenderer(AttractModeAssets assets)
    : assets_(std::move(assets)), car_atlas_(CarAtlas::from_archive(assets_.cars)) {}

FrameBuffer320x200 ReferenceRenderer::render_scene(const RenderScene& scene) const {
    return render_scene_with_debug(scene, DebugViewMode::Off);
}

FrameBuffer320x200 ReferenceRenderer::render_scene_with_debug(
    const RenderScene& scene, DebugViewMode debug_view) const {
    FrameBuffer320x200 frame;
    switch (scene.tag) {
        case RenderScene::Tag::Intro: render_intro(frame, scene.intro); break;
        case RenderScene::Tag::MainMenu: render_main_menu(frame, scene.main_menu); break;
        case RenderScene::Tag::HelpMenu: render_help_menu(frame, scene.help_menu); break;
        case RenderScene::Tag::SettingsMenu:
            render_settings_menu(frame, scene.settings_menu);
            break;
        case RenderScene::Tag::GoMenu:
            render_go_menu(frame, scene.go_menu);
            break;
        case RenderScene::Tag::DemoPlayback:
        case RenderScene::Tag::Gameplay:
            render_play_scene_with_debug(frame, scene.play, debug_view);
            break;
    }
    return frame;
}

void ReferenceRenderer::render_intro(FrameBuffer320x200& frame,
                                     const IntroSequenceState& scene) const {
    frame.clear(RgbColor(0, 0, 0));
    draw_archive_frame(frame, assets_.intro, 0, 1.0f, scene.background_brightness);
    if (scene.anim_frame_index) {
        const std::size_t frame_index =
            std::min(*scene.anim_frame_index, sat_sub(assets_.anim.frames.size(), 1));
        draw_archive_frame(frame, assets_.anim, frame_index, 1.0f, 1.0f);
    }
    if (scene.credit_frame_index) {
        const std::size_t intro_index =
            std::min(*scene.credit_frame_index + 2, sat_sub(assets_.intro.frames.size(), 1));
        draw_archive_frame(frame, assets_.intro, intro_index, scene.credit_alpha, 1.0f);
    }
    if (scene.title_progress > 0.0f) {
        draw_archive_frame_reveal(frame, assets_.intro, 1, scene.title_progress, 1.0f);
    }
    if (scene.title_progress >= 0.98f && !scene.credit_frame_index) {
        draw_branding(frame, 186, 1, 0.8f);
    }
}

void ReferenceRenderer::render_main_menu(FrameBuffer320x200& frame,
                                         const MainMenuScene& scene) const {
    frame.clear(RgbColor(0, 0, 0));
    draw_archive_frame(frame, assets_.intro, 0, 1.0f, 1.0f);
    draw_archive_frame(frame, assets_.intro, 1, 1.0f, 1.0f);
    draw_archive_frame(frame, assets_.main_menu,
                       skyroads::core::menu_cursor_index(scene.selected), 1.0f, 1.0f);
    draw_branding(frame, 184, 2, 1.0f);
}

void ReferenceRenderer::render_help_menu(FrameBuffer320x200& frame,
                                         const HelpMenuScene& scene) const {
    frame.clear(RgbColor(0, 0, 0));
    const std::size_t page_index =
        std::min(scene.page_index, sat_sub(assets_.help_menu.frames.size(), 1));
    draw_archive_frame(frame, assets_.help_menu, page_index, 1.0f, 1.0f);
}

void ReferenceRenderer::render_settings_menu(FrameBuffer320x200& frame,
                                             const SettingsMenuScene& scene) const {
    frame.clear(RgbColor(0, 0, 0));
    const std::size_t frame_index =
        std::min(scene.frame_index, sat_sub(assets_.settings_menu.frames.size(), 1));
    draw_archive_frame(frame, assets_.settings_menu, frame_index, 1.0f, 1.0f);
}

void ReferenceRenderer::render_go_menu(FrameBuffer320x200& frame,
                                       const GoMenuScene& scene) const {
    // Frame 0 of GOMENU is the full stage-selector grid (10 worlds x 3 roads,
    // names and planet thumbnails baked in). We just overlay a cursor on the
    // selected entry. Grid geometry measured from the art: two columns
    // (worlds 0-4 left, 5-9 right), rows step 39px, roads step 9px, first road
    // text at y=13; road text spans x59-92 (left) / x219-252 (right).
    frame.clear(RgbColor(0, 0, 0));
    draw_archive_frame(frame, assets_.go_menu, 0, 1.0f, 1.0f);

    const std::size_t world = scene.selected_world;
    const bool right_col = world >= 5;
    const int row = static_cast<int>(world % 5);
    const int road = static_cast<int>(scene.selected_level);
    const int text_x0 = right_col ? 219 : 59;
    const int text_x1 = right_col ? 252 : 92;
    const int text_y = 13 + row * 39 + road * 9;

    // White outline box around the selected "Road N", plus an orange marker dot.
    stroke_rect(frame, text_x0 - 3, text_y - 2, (text_x1 - text_x0) + 6, 9,
                RgbColor(232, 232, 245));
    frame.fill_rect(text_x1 + 6, text_y + 1, 4, 4, RgbColor(240, 150, 60));
}

void ReferenceRenderer::render_play_scene(FrameBuffer320x200& frame,
                                          const DemoPlaybackState& scene) const {
    const DerivedShipVisualState ship_visual = derive_ship_visual_state(scene);
    const ShipScreenPlacement ship_placement = ship_screen_placement(scene, ship_visual);
    g_road_palette =
        scene.road_palette.empty() ? nullptr : &scene.road_palette;
    frame.clear(RgbColor(0, 0, 0));
    const ImageArchive* world = nullptr;
    if (scene.world_index < assets_.worlds.size()) world = &assets_.worlds[scene.world_index];
    else if (!assets_.worlds.empty()) world = &assets_.worlds.front();
    if (world) draw_archive_frame(frame, *world, 0, 1.0f, 1.0f);

    const bool drew_dos_road = draw_demo_rows_before_ship(frame, scene);
    if (!drew_dos_road) draw_demo_rows_fallback(frame, scene);
    draw_ship_shadow(frame, ship_visual, ship_placement);
    draw_ship_sprite(frame, scene.frame_index, ship_visual, ship_placement);
    if (drew_dos_road) draw_demo_rows_after_ship(frame, scene);
    draw_archive_frame(frame, assets_.dashboard, 0, 1.0f, 1.0f);
    draw_gauge(frame, assets_.oxygen_gauge, scene.snapshot.oxygen_percent);
    draw_gauge(frame, assets_.fuel_gauge, scene.snapshot.fuel_percent);
    const double speed = scene.snapshot.z_velocity / (0x2AAA / 65536.0);
    draw_gauge(frame, assets_.speed_gauge, speed);
    // Result indicators (not the GOMENU art — that is the level-select screen).
    if (!scene.is_demo) {
        if (scene.did_win) {
            draw_text_centered(frame, "LEVEL COMPLETE", 56, RgbColor(150, 255, 170), 2);
            draw_text_centered(frame, "PRESS ENTER", 76, RgbColor(220, 235, 255), 1);
        } else if (scene.snapshot.craft_state != ShipState::Alive &&
                   scene.death_animation_finished) {
            // The five outcomes the EXE distinguishes (ds:0x457c, NOTES Update 9).
            const char* reason = "CRASHED";
            switch (scene.snapshot.craft_state) {
                case ShipState::Fallen: reason = "FELL OFF THE ROAD"; break;
                case ShipState::OutOfFuel: reason = "OUT OF FUEL"; break;
                case ShipState::OutOfOxygen: reason = "OUT OF OXYGEN"; break;
                default: break;
            }
            draw_text_centered(frame, reason, 56, RgbColor(255, 110, 110), 2);
            draw_text_centered(frame, "PRESS ENTER", 76, RgbColor(220, 235, 255), 1);
        }
    }
}

void ReferenceRenderer::render_play_scene_with_debug(FrameBuffer320x200& frame,
                                                     const DemoPlaybackState& scene,
                                                     DebugViewMode debug_view) const {
    switch (debug_view) {
        case DebugViewMode::Off:
            render_play_scene(frame, scene);
            break;
        case DebugViewMode::Overlay:
            render_play_scene(frame, scene);
            draw_debug_overlay(frame, scene);
            break;
        case DebugViewMode::Geometry:
            render_play_geometry_debug(frame, scene);
            break;
        case DebugViewMode::TopDown:
            render_play_topdown_debug(frame, scene);
            break;
    }
}

bool ReferenceRenderer::draw_demo_rows_before_ship(FrameBuffer320x200& frame,
                                                   const DemoPlaybackState& scene) const {
    const std::size_t slot = scene.current_row & 7;
    if (slot >= assets_.trekdat.records.size()) return false;
    return draw_dos_trekdat_pass(frame, scene, assets_.trekdat.records[slot],
                                 DosRoadPhase::BeforeShip);
}

void ReferenceRenderer::draw_demo_rows_after_ship(FrameBuffer320x200& frame,
                                                  const DemoPlaybackState& scene) const {
    const std::size_t slot = scene.current_row & 7;
    if (slot >= assets_.trekdat.records.size()) return;
    draw_dos_trekdat_pass(frame, scene, assets_.trekdat.records[slot],
                          DosRoadPhase::AfterShip);
}

void ReferenceRenderer::draw_demo_rows_fallback(FrameBuffer320x200& frame,
                                                const DemoPlaybackState& scene) const {
    for (const auto& slice : project_road_slices(scene)) {
        draw_projected_slice(frame, slice);
    }
}

void ReferenceRenderer::draw_ship_sprite(FrameBuffer320x200& frame,
                                         std::size_t frame_index,
                                         const DerivedShipVisualState& visual,
                                         ShipScreenPlacement placement) const {
    if (!car_atlas_) return;
    // EXE @0xc18: once the explosion counter passes the 14th frame the sprite
    // index becomes -1, i.e. the wreck stops being drawn at all.
    if (visual.sprite_kind == ShipSpriteKind::Exploding &&
        visual.explosion_frame >= car_atlas_->explosion_frames.size()) {
        return;
    }
    const ImageFrame& sprite = car_atlas_->select_sprite(visual, frame_index);
    const std::size_t draw_width = static_cast<std::size_t>(sprite.width) * SHIP_SCALE;
    const std::size_t draw_height = static_cast<std::size_t>(sprite.height) * SHIP_SCALE;
    const int32_t x = placement.sprite_center_x - (static_cast<int32_t>(draw_width) / 2);
    const int32_t explode_offset =
        visual.sprite_kind == ShipSpriteKind::Exploding ? -2 : 0;
    const int32_t y = placement.sprite_center_y + explode_offset -
                      (static_cast<int32_t>(draw_height) / 2);
    draw_sprite(frame, sprite, x, y, SHIP_SCALE);
}

void ReferenceRenderer::draw_gauge(FrameBuffer320x200& frame,
                                   const HudFragmentPack& pack, double amount) const {
    if (pack.fragments.empty()) return;
    const double clamped = std::clamp(amount, 0.0, 1.0);
    const int64_t rounded =
        static_cast<int64_t>(std::round(clamped * static_cast<double>(pack.fragments.size())));
    const int64_t idx = std::clamp<int64_t>(
        rounded - 1, 0, static_cast<int64_t>(pack.fragments.size()) - 1);
    const auto& fragment = pack.fragments[static_cast<std::size_t>(idx)];
    for (std::size_t y = 0; y < fragment.height; ++y) {
        for (std::size_t x = 0; x < fragment.width; ++x) {
            const uint8_t pixel_index = fragment.pixels[y * fragment.width + x];
            if (pixel_index == 0) continue;
            const std::size_t color_index = std::min<uint8_t>(pixel_index, 2);
            frame.set_pixel(static_cast<std::size_t>(fragment.x) + x,
                            static_cast<std::size_t>(fragment.y) + y,
                            dashboard_colors()[color_index]);
        }
    }
}

void ReferenceRenderer::draw_archive_frame(FrameBuffer320x200& frame,
                                           const ImageArchive& archive,
                                           std::size_t frame_index, float alpha,
                                           float brightness) const {
    if (frame_index >= archive.frames.size()) return;
    for (const auto& fragment : archive.frames[frame_index]) {
        draw_fragment(frame, fragment, alpha, brightness, 1.0f);
    }
}

void ReferenceRenderer::draw_archive_frame_reveal(FrameBuffer320x200& frame,
                                                  const ImageArchive& archive,
                                                  std::size_t frame_index,
                                                  float progress,
                                                  float brightness) const {
    if (frame_index >= archive.frames.size()) return;
    for (const auto& fragment : archive.frames[frame_index]) {
        const uint16_t reveal_width = static_cast<uint16_t>(
            std::round(static_cast<float>(fragment.width) * std::clamp(progress, 0.0f, 1.0f)));
        const float frac =
            static_cast<float>(std::max<uint16_t>(reveal_width, 1)) /
            static_cast<float>(fragment.width);
        draw_fragment(frame, fragment, 1.0f, brightness, frac);
    }
}

void ReferenceRenderer::draw_fragment(FrameBuffer320x200& frame,
                                      const ImageFrame& fragment, float alpha,
                                      float brightness, float horizontal_fraction) const {
    const std::size_t draw_width = static_cast<std::size_t>(std::floor(
        static_cast<float>(fragment.width) * std::clamp(horizontal_fraction, 0.0f, 1.0f)));
    for (std::size_t y = 0; y < fragment.height; ++y) {
        for (std::size_t x = 0; x < draw_width; ++x) {
            const uint8_t pixel_index = fragment.pixels[y * fragment.width + x];
            if (fragment.transparent_zero && pixel_index == 0) continue;
            if (pixel_index >= fragment.palette.colors.size()) continue;
            const RgbColor color =
                scale_brightness(fragment.palette.colors[pixel_index], brightness);
            frame.blend_pixel(static_cast<std::size_t>(fragment.x_offset) + x,
                              static_cast<std::size_t>(fragment.y_offset) + y, color,
                              alpha);
        }
    }
}

void ReferenceRenderer::draw_sprite(FrameBuffer320x200& frame,
                                    const ImageFrame& sprite, int32_t dest_x,
                                    int32_t dest_y, std::size_t scale) const {
    for (std::size_t y = 0; y < sprite.height; ++y) {
        for (std::size_t x = 0; x < sprite.width; ++x) {
            const uint8_t pixel_index = sprite.pixels[y * sprite.width + x];
            if (sprite.transparent_zero && pixel_index == 0) continue;
            if (pixel_index >= sprite.palette.colors.size()) continue;
            const RgbColor color = sprite.palette.colors[pixel_index];
            for (std::size_t sy = 0; sy < scale; ++sy) {
                for (std::size_t sx = 0; sx < scale; ++sx) {
                    const int32_t px = dest_x + static_cast<int32_t>(x * scale + sx);
                    const int32_t py = dest_y + static_cast<int32_t>(y * scale + sy);
                    if (px < 0 || py < 0) continue;
                    frame.set_pixel(static_cast<std::size_t>(px),
                                    static_cast<std::size_t>(py), color);
                }
            }
        }
    }
}

void ReferenceRenderer::draw_projected_slice(FrameBuffer320x200& frame,
                                             const ProjectedRoadSlice& slice) const {
    const std::size_t height = std::max<std::size_t>(sat_sub(slice.bottom_y, slice.top_y), 1);
    for (std::size_t y = slice.top_y; y < std::min(slice.bottom_y, VIEW_BOTTOM_Y); ++y) {
        const float t = static_cast<float>(y - slice.top_y) / static_cast<float>(height);
        const float center = lerp(slice.center_top, slice.center_bottom, t);
        const float road_width = lerp(slice.width_top, slice.width_bottom, t);
        for (const auto& span : slice.spans) {
            const int32_t x0 =
                project_span_x(center, road_width, span.top_start, span.bottom_start, t);
            const int32_t x1 =
                project_span_x(center, road_width, span.top_end, span.bottom_end, t);
            const int32_t width = std::max(x1 - x0, 1);
            frame.fill_rect(x0, static_cast<int32_t>(y), width, 1, road_color(span.sample_cell));
            const RgbColor edge_color = road_edge_color(span.sample_cell);
            frame.fill_rect(x0, static_cast<int32_t>(y), std::min(2, width), 1, edge_color);
            frame.fill_rect(x0 + width - 1, static_cast<int32_t>(y), 1, 1, edge_color);
        }
        if (slice.tunnel_span) {
            const float left_fraction = slice.tunnel_span->first;
            const float right_fraction = slice.tunnel_span->second;
            const int32_t x0 = static_cast<int32_t>(
                std::round(center - road_width / 2.0f + road_width * left_fraction));
            const int32_t x1 = static_cast<int32_t>(
                std::round(center - road_width / 2.0f + road_width * right_fraction));
            const int32_t tunnel_height = static_cast<int32_t>(
                std::round(static_cast<float>(slice.bottom_y - slice.top_y) * 0.75f));
            frame.fill_rect(x0, static_cast<int32_t>(y) - tunnel_height,
                            std::max(x1 - x0, 1), 1, RgbColor(84, 60, 48));
        }
    }

    for (const auto& obstacle : slice.obstacles) {
        const float near_center = slice.center_bottom;
        const float near_width = slice.width_bottom;
        const int32_t x0 = static_cast<int32_t>(std::round(
            near_center - near_width / 2.0f + near_width * obstacle.column_start));
        const int32_t x1 = static_cast<int32_t>(std::round(
            near_center - near_width / 2.0f + near_width * obstacle.column_end));
        const int32_t width = std::max(x1 - x0, 2);
        const int32_t obstacle_height = static_cast<int32_t>(std::round(
            static_cast<float>(slice.bottom_y - slice.top_y) *
            (1.5f + obstacle.height_factor * 1.5f)));
        const int32_t y_top = static_cast<int32_t>(slice.top_y) - obstacle_height;
        frame.fill_rect(x0, y_top, width, obstacle_height, obstacle.color);
        frame.fill_rect(x0, y_top, width, 2, scale_brightness(obstacle.color, 1.2f));
        frame.fill_rect(x0 + width - 2, y_top, 2, obstacle_height,
                        scale_brightness(obstacle.color, 0.7f));
    }
}

void ReferenceRenderer::draw_ship_shadow(FrameBuffer320x200& frame,
                                         const DerivedShipVisualState& visual,
                                         ShipScreenPlacement placement) const {
    // Draw the ground shadow whenever alive (including mid-jump) — the original
    // keeps it on the ground below the ship. It shrinks with height via `hover`.
    if (!visual.casts_shadow) return;
    const int32_t shadow_center_x = placement.shadow_center_x;
    const int32_t shadow_center_y = placement.shadow_center_y;
    const int32_t hover = std::max(-visual.vertical_offset_y, 0);
    const int32_t radius_x = std::clamp(9 - hover / 6, 4, 9);
    const int32_t radius_y = std::clamp(4 - hover / 10, 2, 4);
    for (int32_t dy = -radius_y; dy <= radius_y; ++dy) {
        for (int32_t dx = -radius_x; dx <= radius_x; ++dx) {
            const int32_t ellipse = (dx * dx * 100) / (radius_x * radius_x) +
                                    (dy * dy * 100) / (radius_y * radius_y);
            if (ellipse > 100) continue;
            const int32_t px = shadow_center_x + dx;
            const int32_t py = shadow_center_y + dy;
            if (px < 0 || py < static_cast<int32_t>(HORIZON_Y) ||
                py >= static_cast<int32_t>(VIEW_BOTTOM_Y)) {
                continue;
            }
            frame.blend_pixel(static_cast<std::size_t>(px),
                              static_cast<std::size_t>(py), RgbColor(0, 0, 0), 0.18f);
        }
    }
}

void ReferenceRenderer::draw_branding(FrameBuffer320x200& frame, int32_t y,
                                      std::size_t scale, float alpha) const {
    const std::string text = "SKYROADS NATIVE SDL PORT";
    const RgbColor color = scale_brightness(RgbColor(245, 214, 109), alpha);
    const RgbColor shadow = scale_brightness(RgbColor(54, 24, 70), alpha);
    draw_text_centered(frame, text, y + static_cast<int32_t>(scale), shadow, scale);
    draw_text_centered(frame, text, y, color, scale);
}

void ReferenceRenderer::draw_text_centered(FrameBuffer320x200& frame,
                                           const std::string& text, int32_t y,
                                           RgbColor color, std::size_t scale) const {
    const int32_t width = text_pixel_width(text, scale);
    const int32_t x = (static_cast<int32_t>(FRAMEBUFFER_WIDTH) - width) / 2;
    draw_text(frame, x, y, text, color, scale);
}

void ReferenceRenderer::draw_text(FrameBuffer320x200& frame, int32_t x, int32_t y,
                                  const std::string& text, RgbColor color,
                                  std::size_t scale) const {
    int32_t cursor = x;
    for (char ch : text) {
        if (ch == ' ') {
            cursor += static_cast<int32_t>(4 * scale);
            continue;
        }
        auto rows = glyph_rows(ch);
        if (!rows) {
            cursor += static_cast<int32_t>(4 * scale);
            continue;
        }
        for (std::size_t row_index = 0; row_index < 5; ++row_index) {
            const uint8_t row_bits = (*rows)[row_index];
            for (std::size_t col_index = 0; col_index < 3; ++col_index) {
                if (((row_bits >> (2 - col_index)) & 1) == 0) continue;
                frame.fill_rect(cursor + static_cast<int32_t>(col_index * scale),
                                y + static_cast<int32_t>(row_index * scale),
                                static_cast<int32_t>(scale),
                                static_cast<int32_t>(scale), color);
            }
        }
        cursor += static_cast<int32_t>(4 * scale);
    }
}

void ReferenceRenderer::draw_debug_overlay(FrameBuffer320x200& frame,
                                           const DemoPlaybackState& scene) const {
    const DerivedShipVisualState visual = derive_ship_visual_state(scene);
    const std::vector<ProjectedRoadSlice> slices = project_road_slices(scene);
    const ShipScreenPlacement placement =
        ship_screen_placement_from_slices(scene, visual, slices);
    draw_debug_hud_panel(frame, scene, DebugViewMode::Overlay);
    draw_projected_slice_guides(frame, slices);
    draw_ship_debug_guides(frame, scene, visual, placement);
    draw_topdown_inset(frame, scene);
}

void ReferenceRenderer::render_play_geometry_debug(FrameBuffer320x200& frame,
                                                   const DemoPlaybackState& scene) const {
    const DerivedShipVisualState visual = derive_ship_visual_state(scene);
    const std::vector<ProjectedRoadSlice> slices = project_road_slices(scene);
    const ShipScreenPlacement placement =
        ship_screen_placement_from_slices(scene, visual, slices);
    frame.clear(RgbColor(8, 8, 14));
    const ImageArchive* world = nullptr;
    if (scene.world_index < assets_.worlds.size()) world = &assets_.worlds[scene.world_index];
    else if (!assets_.worlds.empty()) world = &assets_.worlds.front();
    if (world) draw_archive_frame(frame, *world, 0, 0.25f, 0.45f);
    frame.fill_rect(0, static_cast<int32_t>(HORIZON_Y),
                    static_cast<int32_t>(FRAMEBUFFER_WIDTH),
                    static_cast<int32_t>(VIEW_BOTTOM_Y - HORIZON_Y), RgbColor(10, 10, 20));
    for (const auto& slice : slices) draw_projected_slice(frame, slice);
    draw_projected_slice_guides(frame, slices);
    draw_ship_shadow(frame, visual, placement);
    draw_ship_sprite(frame, scene.frame_index, visual, placement);
    draw_ship_debug_guides(frame, scene, visual, placement);
    draw_topdown_inset(frame, scene);
    draw_archive_frame(frame, assets_.dashboard, 0, 1.0f, 1.0f);
    draw_debug_hud_panel(frame, scene, DebugViewMode::Geometry);
}

void ReferenceRenderer::render_play_topdown_debug(FrameBuffer320x200& frame,
                                                  const DemoPlaybackState& scene) const {
    frame.clear(RgbColor(6, 6, 10));
    const ImageArchive* world = nullptr;
    if (scene.world_index < assets_.worlds.size()) world = &assets_.worlds[scene.world_index];
    else if (!assets_.worlds.empty()) world = &assets_.worlds.front();
    if (world) draw_archive_frame(frame, *world, 0, 0.20f, 0.40f);
    frame.fill_rect(12, 18, static_cast<int32_t>(FRAMEBUFFER_WIDTH) - 24,
                    static_cast<int32_t>(VIEW_BOTTOM_Y - 26), RgbColor(16, 18, 26));
    draw_topdown_map(frame, scene, 20, 26, 280, 104, true);
    draw_archive_frame(frame, assets_.dashboard, 0, 1.0f, 1.0f);
    draw_debug_hud_panel(frame, scene, DebugViewMode::TopDown);
}

void ReferenceRenderer::draw_debug_hud_panel(FrameBuffer320x200& frame,
                                             const DemoPlaybackState& scene,
                                             DebugViewMode mode) const {
    frame.fill_rect(DEBUG_PANEL_X, DEBUG_PANEL_Y, DEBUG_PANEL_W, DEBUG_PANEL_H,
                    RgbColor(10, 12, 18));
    stroke_rect(frame, DEBUG_PANEL_X, DEBUG_PANEL_Y, DEBUG_PANEL_W, DEBUG_PANEL_H,
                RgbColor(82, 196, 230));
    const auto row_state = renderer_row_state(static_cast<uint16_t>(scene.current_row));
    draw_text(frame, DEBUG_PANEL_X + 4, DEBUG_PANEL_Y + 4, debug_label(mode),
              RgbColor(244, 233, 146), 1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ROW %03zu", scene.current_row);
    draw_text(frame, DEBUG_PANEL_X + 4, DEBUG_PANEL_Y + 13, buf, RgbColor(190, 220, 255), 1);
    std::snprintf(buf, sizeof(buf), "GRP %02zu SLT %zu", row_state.road_row_group,
                  row_state.trekdat_slot);
    draw_text(frame, DEBUG_PANEL_X + 4, DEBUG_PANEL_Y + 22, buf, RgbColor(190, 220, 255), 1);
    draw_text(frame, DEBUG_PANEL_X + 4, DEBUG_PANEL_Y + 31,
              short_ship_state(scene.ship.state), RgbColor(247, 160, 160), 1);
}

void ReferenceRenderer::draw_projected_slice_guides(
    FrameBuffer320x200& frame, const std::vector<ProjectedRoadSlice>& slices) const {
    for (const auto& slice : slices) {
        const int32_t left_top =
            static_cast<int32_t>(std::round(slice.center_top - slice.width_top / 2.0f));
        const int32_t right_top =
            static_cast<int32_t>(std::round(slice.center_top + slice.width_top / 2.0f));
        const int32_t left_bottom =
            static_cast<int32_t>(std::round(slice.center_bottom - slice.width_bottom / 2.0f));
        const int32_t right_bottom =
            static_cast<int32_t>(std::round(slice.center_bottom + slice.width_bottom / 2.0f));
        frame.fill_rect(left_top, static_cast<int32_t>(slice.top_y), 1, 1, RgbColor(110, 255, 170));
        frame.fill_rect(right_top, static_cast<int32_t>(slice.top_y), 1, 1, RgbColor(110, 255, 170));
        frame.fill_rect(left_bottom, static_cast<int32_t>(sat_sub(slice.bottom_y, 1)), 1, 1,
                        RgbColor(110, 255, 170));
        frame.fill_rect(right_bottom, static_cast<int32_t>(sat_sub(slice.bottom_y, 1)), 1, 1,
                        RgbColor(110, 255, 170));
        for (const auto& obstacle : slice.obstacles) {
            const int32_t x0 = static_cast<int32_t>(std::round(
                slice.center_bottom - slice.width_bottom / 2.0f +
                slice.width_bottom * obstacle.column_start));
            const int32_t x1 = static_cast<int32_t>(std::round(
                slice.center_bottom - slice.width_bottom / 2.0f +
                slice.width_bottom * obstacle.column_end));
            const int32_t height = static_cast<int32_t>(std::round(
                static_cast<float>(slice.bottom_y - slice.top_y) *
                (1.5f + obstacle.height_factor * 1.5f)));
            const int32_t y0 = static_cast<int32_t>(slice.top_y) - height;
            stroke_rect(frame, x0, y0, std::max(x1 - x0, 2), std::max(height, 2),
                        RgbColor(255, 122, 122));
        }
        if (slice.tunnel_span) {
            const int32_t x0 = static_cast<int32_t>(std::round(
                slice.center_bottom - slice.width_bottom / 2.0f +
                slice.width_bottom * slice.tunnel_span->first));
            const int32_t x1 = static_cast<int32_t>(std::round(
                slice.center_bottom - slice.width_bottom / 2.0f +
                slice.width_bottom * slice.tunnel_span->second));
            const int32_t tunnel_height = static_cast<int32_t>(std::round(
                static_cast<float>(slice.bottom_y - slice.top_y) * 0.75f));
            frame.fill_rect(x0, static_cast<int32_t>(slice.top_y) - tunnel_height,
                            std::max(x1 - x0, 1), 1, RgbColor(255, 179, 87));
        }
    }
}

void ReferenceRenderer::draw_ship_debug_guides(FrameBuffer320x200& frame,
                                               const DemoPlaybackState& scene,
                                               const DerivedShipVisualState& visual,
                                               ShipScreenPlacement placement) const {
    if (!car_atlas_) return;
    const ImageFrame& sprite = car_atlas_->select_sprite(visual, scene.frame_index);
    const std::size_t draw_width = static_cast<std::size_t>(sprite.width) * SHIP_SCALE;
    const std::size_t draw_height = static_cast<std::size_t>(sprite.height) * SHIP_SCALE;
    const int32_t x = placement.sprite_center_x - (static_cast<int32_t>(draw_width) / 2);
    const int32_t y = placement.sprite_center_y - (static_cast<int32_t>(draw_height) / 2);
    stroke_rect(frame, x, y, static_cast<int32_t>(draw_width),
                static_cast<int32_t>(draw_height), RgbColor(100, 220, 255));
    frame.fill_rect(placement.sprite_center_x - 8, placement.sprite_center_y, 16, 1,
                    RgbColor(255, 230, 120));
    frame.fill_rect(placement.sprite_center_x, placement.sprite_center_y - 8, 1, 16,
                    RgbColor(255, 230, 120));
}

void ReferenceRenderer::draw_topdown_inset(FrameBuffer320x200& frame,
                                           const DemoPlaybackState& scene) const {
    draw_topdown_map(frame, scene, DEBUG_TOPDOWN_INSET_X, DEBUG_TOPDOWN_INSET_Y,
                     DEBUG_TOPDOWN_INSET_W, DEBUG_TOPDOWN_INSET_H, false);
}

void ReferenceRenderer::draw_topdown_map(FrameBuffer320x200& frame,
                                         const DemoPlaybackState& scene, int32_t x,
                                         int32_t y, int32_t w, int32_t h,
                                         bool large) const {
    frame.fill_rect(x, y, w, h, RgbColor(8, 10, 14));
    stroke_rect(frame, x, y, w, h, RgbColor(82, 196, 230));
    if (scene.rows.empty()) return;
    const int32_t row_h = std::max(h - 8, 7) /
                          std::max<int32_t>(static_cast<int32_t>(scene.rows.size()), 1);
    const int32_t col_w = (w - 8) / static_cast<int32_t>(ROAD_COLUMNS);
    const double left_edge = LEVEL_CENTER_X - LEVEL_TILE_STRIDE_X * 3.5;
    for (std::size_t row_idx = 0; row_idx < scene.rows.size(); ++row_idx) {
        const RoadRenderRow& row = scene.rows[row_idx];
        const int32_t cell_y = y + 4 + static_cast<int32_t>(row_idx) * row_h;
        for (std::size_t col_idx = 0; col_idx < row.cells.size(); ++col_idx) {
            const int32_t cell_x = x + 4 + static_cast<int32_t>(col_idx) * col_w;
            frame.fill_rect(cell_x, cell_y, std::max(col_w, 2) - 1, std::max(row_h, 2) - 1,
                            debug_cell_color(row.cells[col_idx]));
            if (row.cells[col_idx].has_tunnel) {
                stroke_rect(frame, cell_x + 1, cell_y + 1, std::max(std::max(col_w, 3) - 3, 1),
                            std::max(std::max(row_h, 3) - 3, 1), RgbColor(255, 178, 90));
            }
        }
        if (row.row_index == (scene.current_row >> 3)) {
            stroke_rect(frame, x + 3, cell_y - 1, w - 6, std::max(row_h, 2) + 1,
                        RgbColor(255, 240, 120));
        }
    }
    const double row_start =
        scene.rows.empty() ? 0.0 : static_cast<double>(scene.rows.front().row_index);
    const double row_span = std::max<std::size_t>(scene.rows.size(), 1);
    const double ship_row =
        std::clamp((scene.ship.z_position - row_start) / row_span, 0.0, 0.999);
    const double ship_col = std::clamp(
        (scene.ship.x_position - left_edge) /
            (LEVEL_TILE_STRIDE_X * static_cast<double>(ROAD_COLUMNS)),
        0.0, 0.999);
    const int32_t ship_x =
        x + 4 + static_cast<int32_t>(ship_col * static_cast<double>(std::max(w - 8, 1)));
    const int32_t ship_y =
        y + 4 + static_cast<int32_t>(ship_row * static_cast<double>(std::max(h - 8, 1)));
    frame.fill_rect(ship_x - 2, ship_y - 2, 5, 5, RgbColor(112, 214, 255));
    if (large) {
        draw_text(frame, x + 4, y - 10, "TOPDOWN", RgbColor(244, 233, 146), 1);
    }
}

uint64_t frame_hash(const FrameBuffer320x200& frame) {
    uint64_t acc = 0;
    for (uint8_t value : frame.pixels_rgba) {
        acc = acc * 16777619ull + static_cast<uint64_t>(value);
    }
    return acc;
}

} // namespace skyroads::renderer
