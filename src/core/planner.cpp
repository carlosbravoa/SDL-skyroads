#include "core/planner.hpp"

namespace skyroads::core {

using skyroads::data::analyze_road_descriptor;
using skyroads::data::DEMO_TILE_POSITION_STEP_FP16;
using skyroads::data::DemoInput;
using skyroads::data::DemoRecording;
using skyroads::data::RoadDescriptor;
using skyroads::data::RoadEntry;
using skyroads::data::RoadRow;
using skyroads::data::ROAD_COLUMNS;

std::size_t demo_index_for_z_position(uint32_t z_position_fp16) {
    return z_position_fp16 / DEMO_TILE_POSITION_STEP_FP16;
}

DemoCursor demo_cursor(uint32_t z_position_fp16) {
    return DemoCursor{z_position_fp16, demo_index_for_z_position(z_position_fp16)};
}

const DemoInput* sample_demo_input(const DemoRecording& demo,
                                   uint32_t z_position_fp16) {
    const std::size_t index = demo_index_for_z_position(z_position_fp16);
    if (index >= demo.entries.size()) {
        return nullptr;
    }
    return &demo.entries[index];
}

RendererRowState renderer_row_state(uint16_t current_row) {
    return RendererRowState{current_row,
                            static_cast<std::size_t>(current_row >> 3),
                            static_cast<std::size_t>(current_row & 0x0007)};
}

RendererCellPlan plan_renderer_cell(uint16_t current_row, uint16_t descriptor_raw) {
    const RendererRowState row_state = renderer_row_state(current_row);
    const RoadDescriptor descriptor = analyze_road_descriptor(descriptor_raw);
    // Baked DOS runtime tables (see dos_render_tables.hpp) — no EXE needed.
    const uint8_t tile_class = dos_tile_class(descriptor.dispatch_variant_low3);
    const DispatchEntry dispatch = dos_dispatch_entry(descriptor.dispatch_kind);

    return RendererCellPlan{current_row,   row_state.road_row_group,
                            row_state.trekdat_slot, descriptor,
                            tile_class,    dispatch};
}

RendererRowPlan plan_renderer_row(uint16_t current_row, const RoadRow& road_row) {
    const RendererRowState row_state = renderer_row_state(current_row);
    RendererRowPlan plan;
    plan.current_row = current_row;
    plan.road_row_group = row_state.road_row_group;
    plan.trekdat_slot = row_state.trekdat_slot;
    for (std::size_t column = 0; column < ROAD_COLUMNS; ++column) {
        plan.cells[column] = plan_renderer_cell(current_row, road_row[column]);
    }
    return plan;
}

std::optional<GameplayFramePlan> plan_gameplay_frame(
    const DemoRecording& demo, const RoadEntry& road, std::size_t road_row_index,
    uint16_t current_row, uint32_t z_position_fp16) {
    if (road_row_index >= road.rows.size()) {
        return std::nullopt;
    }
    const RoadRow& road_row = road.rows[road_row_index];
    const DemoCursor cursor = demo_cursor(z_position_fp16);
    const DemoInput* demo_input =
        cursor.index < demo.entries.size() ? &demo.entries[cursor.index]
                                           : nullptr;

    GameplayFramePlan plan;
    plan.z_position_fp16 = z_position_fp16;
    plan.demo_cursor = cursor;
    plan.demo_input = demo_input;
    plan.road_index = road.index;
    plan.road_row_index = road_row_index;
    plan.renderer_row = plan_renderer_row(current_row, road_row);
    return plan;
}

} // namespace skyroads::core
