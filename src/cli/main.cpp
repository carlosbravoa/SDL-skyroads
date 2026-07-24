// Part of the SkyRoads SDL port
//
// Verification/inspection CLI: `summary <dir>` prints the native baseline for a
// SkyRoads data directory; `demo-sim <dir> [frames]` runs the deterministic
// demo and prints per-frame ship state (the same text trace the reference CLI emits).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/core.hpp"
#include "data/data.hpp"

using namespace skyroads::data;
using skyroads::core::GameplayEvent;
using skyroads::core::GameplayFrameResult;
using skyroads::core::GameplaySession;
using skyroads::core::ShipState;

namespace {

std::string join_u8s(const std::vector<uint8_t>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(values[i]);
    }
    return out;
}

std::string join_i8_counts(const std::map<int8_t, std::size_t>& counts) {
    std::string out;
    bool first = true;
    for (const auto& [key, value] : counts) {
        if (!first) out += " ";
        first = false;
        out += std::to_string(static_cast<int>(key)) + "=" + std::to_string(value);
    }
    return out;
}

const char* ship_state_name(ShipState state) {
    switch (state) {
        case ShipState::Alive: return "Alive";
        case ShipState::Exploded: return "Exploded";
        case ShipState::Fallen: return "Fallen";
        case ShipState::OutOfFuel: return "OutOfFuel";
        case ShipState::OutOfOxygen: return "OutOfOxygen";
    }
    return "?";
}

std::string join_events(const std::vector<GameplayEvent>& events) {
    if (events.empty()) return "-";
    std::string out;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i) out += ",";
        switch (events[i]) {
            case GameplayEvent::ShipBumpedWall: out += "ShipBumpedWall"; break;
            case GameplayEvent::ShipExploded: out += "ShipExploded"; break;
            case GameplayEvent::ShipBounced: out += "ShipBounced"; break;
            case GameplayEvent::ShipRefilled: out += "ShipRefilled"; break;
        }
    }
    return out;
}

std::string join_path(const std::string& root, const char* name) {
    if (!root.empty() && root.back() == '/') return root + name;
    return root + "/" + name;
}

int summary(const std::string& root) {
    RoadsArchive roads = load_roads_lzs_path(join_path(root, "ROADS.LZS"));
    DemoRecording demo = load_demo_rec_path(join_path(root, "DEMO.REC"));
    TrekdatArchive trekdat = load_trekdat_lzs_path(join_path(root, "TREKDAT.LZS"));
    MuzaxArchive muzax = load_muzax_lzs_path(join_path(root, "MUZAX.LZS"));
    // The EXE is inspection-only here; the port bakes its runtime tables in and
    // does not need it. Load it if present, otherwise skip that section.
    std::optional<SkyroadsExe> exe_opt;
    try {
        exe_opt = load_skyroads_exe_path(join_path(root, "SKYROADS.EXE"));
    } catch (const Error&) {
        exe_opt = std::nullopt;
    }

    std::printf("SkyRoads Native Baseline\n");
    std::printf("source_root: %s\n\n", root.c_str());

    std::printf("roads:\n");
    std::printf("  road_count: %zu\n", roads.road_count());
    std::printf("  used_dispatch_kinds: %s\n",
                join_u8s(roads.used_dispatch_kinds()).c_str());
    std::printf("  distinct_descriptor_count: %zu\n",
                roads.distinct_descriptor_count());
    for (const auto& entry : roads.descriptor_catalog.dispatch_kinds) {
        std::printf("  dispatch_kind_%u: count=%zu descriptors=%zu\n",
                    entry.dispatch_kind, entry.count, entry.descriptor_count);
    }
    std::printf("\n");

    std::printf("demo:\n");
    std::printf("  byte_count: %zu\n", demo.byte_count());
    std::printf("  approx_tile_length_fp16: 0x%08X\n",
                demo.approx_tile_length_fp16());
    std::printf("  approx_tile_length: %.9f\n", demo.approx_tile_length());
    std::printf("  accelerate_decelerate_counts: %s\n",
                join_i8_counts(demo.accelerate_decelerate_counts).c_str());
    std::printf("  left_right_counts: %s\n",
                join_i8_counts(demo.left_right_counts).c_str());
    std::printf("  jump_counts: false=%zu true=%zu\n",
                demo.jump_counts.false_count, demo.jump_counts.true_count);
    std::printf("\n");

    std::printf("trekdat:\n");
    std::printf("  record_count: %zu\n", trekdat.record_count());
    std::printf("  pointer_grid: 13x24\n");
    std::string expanded, spans, ptrmax;
    for (std::size_t i = 0; i < trekdat.records.size(); ++i) {
        if (i) { expanded += ","; spans += ","; ptrmax += ","; }
        expanded += std::to_string(trekdat.records[i].load_buff_end);
        spans += std::to_string(trekdat.records[i].total_span_count());
        ptrmax += std::to_string(trekdat.records[i].pointer_max());
    }
    std::printf("  expanded_sizes: %s\n", expanded.c_str());
    std::printf("  total_span_counts: %s\n", spans.c_str());
    std::printf("  pointer_maxes: %s\n\n", ptrmax.c_str());

    std::printf("muzax:\n");
    std::printf("  song_table_size: %u\n", muzax.song_table_size);
    std::printf("  song_count: %zu\n", muzax.song_count());
    std::printf("  populated_song_count: %zu\n", muzax.populated_song_count());
    if (!muzax.songs.empty()) {
        const MuzaxSong& song0 = muzax.songs[0];
        if (song0.widths) {
            std::printf("  song_0_widths: %u,%u,%u\n", (*song0.widths)[0],
                        (*song0.widths)[1], (*song0.widths)[2]);
        }
        std::printf("  song_0_instrument_bytes: %zu\n", song0.instrument_bytes);
        std::printf("  song_0_command_bytes: %zu\n", song0.command_bytes);
        if (song0.command_summary) {
            std::string fc;
            for (std::size_t i = 0; i < 8; ++i) {
                if (i) fc += ",";
                fc += std::to_string(song0.command_summary->function_counts[i]);
            }
            std::printf("  song_0_function_counts: %s\n", fc.c_str());
        }
    }
    std::printf("\n");

    if (!exe_opt) {
        std::printf("exe: (SKYROADS.EXE not present / not this build — skipped; "
                    "runtime tables are baked in)\n");
        return 0;
    }
    const SkyroadsExe& exe = *exe_opt;
    std::printf("exe:\n");
    std::printf("  header_bytes: %zu\n", exe.header_bytes);
    std::printf("  image_size: %zu\n", exe.image_size);
    std::printf("  entry_file_offset: %zu\n", exe.entry_file_offset);
    std::printf("  exe_reader_base_file_offset: %zu\n",
                exe.exe_reader_base_file_offset);
    std::string tc;
    for (std::size_t i = 0; i < exe.runtime_tables.tile_class_by_low3.values.size();
         ++i) {
        if (i) tc += ",";
        tc += std::to_string(exe.runtime_tables.tile_class_by_low3.values[i]);
    }
    std::printf("  tile_class_by_low3: %s\n", tc.c_str());
    std::string dd;
    const auto& entries = exe.runtime_tables.draw_dispatch_by_type.entries;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) dd += ",";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04X", entries[i].target);
        dd += buf;
    }
    std::printf("  draw_dispatch_by_type: %s\n", dd.c_str());
    return 0;
}

int demo_sim(const std::string& root, const std::vector<std::string>& extra) {
    std::size_t frame_count = 60;
    if (!extra.empty()) {
        frame_count = static_cast<std::size_t>(std::strtoul(extra[0].c_str(),
                                                            nullptr, 10));
    }

    RoadsArchive roads = load_roads_lzs_path(join_path(root, "ROADS.LZS"));
    DemoRecording demo = load_demo_rec_path(join_path(root, "DEMO.REC"));
    Level level = level_from_road_entry(roads.roads[0]);
    GameplaySession session(level);

    std::printf("SkyRoads Demo Simulation\n");
    std::printf("source_root: %s\n", root.c_str());
    std::printf("level: %s (index %zu)\n", level.name.c_str(), level.road_index);
    std::printf("gravity: %u fuel: %u oxygen: %u\n", level.gravity, level.fuel,
                level.oxygen);
    std::printf("frames: %zu\n\n", frame_count);

    for (std::size_t i = 0; i < frame_count; ++i) {
        GameplayFrameResult frame = session.run_demo_frame(demo);
        std::printf(
            "frame=%04zu turn=%+d accel=%+d jump=%d row=%zu "
            "pos=(%.6f,%.6f,%.6f) zvel=%.6f oxygen=%.6f fuel=%.6f state=%s "
            "events=%s\n",
            frame.frame_index, frame.controls.turn_input,
            frame.controls.accel_input, frame.controls.jump_input ? 1 : 0,
            frame.road_row_index, frame.snapshot.x_position,
            frame.snapshot.y_position, frame.snapshot.z_position,
            frame.snapshot.z_velocity, frame.snapshot.oxygen_percent,
            frame.snapshot.fuel_percent, ship_state_name(frame.snapshot.craft_state),
            join_events(frame.events).c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string program = argc > 0 ? argv[0] : "skyroads-cli";
    const std::string command = argc > 1 ? argv[1] : "";
    const std::string source_root = argc > 2 ? argv[2] : "";
    std::vector<std::string> extra;
    for (int i = 3; i < argc; ++i) extra.emplace_back(argv[i]);

    try {
        if (command == "summary" && !source_root.empty()) {
            return summary(source_root);
        }
        if (command == "demo-sim" && !source_root.empty()) {
            return demo_sim(source_root, extra);
        }
        std::fprintf(stderr, "usage: %s <summary|demo-sim> <source_root> [args]\n",
                     program.c_str());
        return 2;
    } catch (const Error& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
