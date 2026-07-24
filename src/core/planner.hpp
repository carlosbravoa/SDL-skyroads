// Faithful C++ port of the top-level planning functions
//
// Renderer/demo planning: pure mapping from row counters and road descriptors
// onto the DOS renderer's TREKDAT slot, dispatch, and tile-class selections.
//
// The dispatch/tile-class tables were reverse-engineered from SKYROADS.EXE and
// are now baked into `dos_render_tables.hpp`, so this planner has no runtime
// dependency on the executable (the original design threaded a `SkyroadsExe` in;
// we interpret it once at build/test time and reimplement it as constants).
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "core/dos_render_tables.hpp"
#include "data/demo.hpp"
#include "data/roads.hpp"

namespace skyroads::core {

struct DemoCursor {
    uint32_t z_position_fp16;
    std::size_t index;
};

struct RendererRowState {
    uint16_t current_row;
    std::size_t road_row_group;
    std::size_t trekdat_slot;
};

struct RendererCellPlan {
    uint16_t current_row;
    std::size_t road_row_group;
    std::size_t trekdat_slot;
    skyroads::data::RoadDescriptor descriptor;
    uint8_t tile_class;
    DispatchEntry dispatch;
};

struct RendererRowPlan {
    uint16_t current_row;
    std::size_t road_row_group;
    std::size_t trekdat_slot;
    std::array<RendererCellPlan, skyroads::data::ROAD_COLUMNS> cells;
};

struct GameplayFramePlan {
    uint32_t z_position_fp16;
    DemoCursor demo_cursor;
    const skyroads::data::DemoInput* demo_input; // null == the reference design None
    std::size_t road_index;
    std::size_t road_row_index;
    RendererRowPlan renderer_row;
};

std::size_t demo_index_for_z_position(uint32_t z_position_fp16);
DemoCursor demo_cursor(uint32_t z_position_fp16);
const skyroads::data::DemoInput* sample_demo_input(
    const skyroads::data::DemoRecording& demo, uint32_t z_position_fp16);
RendererRowState renderer_row_state(uint16_t current_row);
RendererCellPlan plan_renderer_cell(uint16_t current_row, uint16_t descriptor_raw);
RendererRowPlan plan_renderer_row(uint16_t current_row,
                                  const skyroads::data::RoadRow& road_row);
std::optional<GameplayFramePlan> plan_gameplay_frame(
    const skyroads::data::DemoRecording& demo,
    const skyroads::data::RoadEntry& road, std::size_t road_row_index,
    uint16_t current_row, uint32_t z_position_fp16);

} // namespace skyroads::core
