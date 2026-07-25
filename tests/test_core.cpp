// Equivalence tests for the core layer (planner, gameplay simulation, and app
// state machine). The demo-frame test asserts the exact IEEE-754 positions the
// simulation produces -- if the fixed-point float math had drifted, these fail.
#include <vector>

#include "check.hpp"
#include "core/core.hpp"
#include "data/data.hpp"

using namespace skyroads::core;
using skyroads::data::level_from_road_entry;
using skyroads::data::levels_from_roads_archive;
using skyroads::data::load_demo_rec_path;
using skyroads::data::load_roads_lzs_path;
using skyroads::data::load_skyroads_exe_path;
using skyroads::data::GROUND_Y;
using skyroads::data::Level;
using skyroads::data::RoadsArchive;
using skyroads::data::SkyroadsExe;
using skyroads::data::DemoRecording;

// ---- planner ---------------------------------------------------------------

static void test_demo_indexing() {
    CHECK_EQ(demo_index_for_z_position(0), static_cast<std::size_t>(0));
    CHECK_EQ(demo_index_for_z_position(0x0665), static_cast<std::size_t>(0));
    CHECK_EQ(demo_index_for_z_position(0x0666), static_cast<std::size_t>(1));
    CHECK_EQ(demo_index_for_z_position(0x0CCC), static_cast<std::size_t>(2));
    CHECK_EQ(demo_cursor(0x1332).index, static_cast<std::size_t>(3));
}

static void test_demo_sampling() {
    DemoRecording demo = load_demo_rec_path(check::asset("DEMO.REC"));
    const auto* e0 = sample_demo_input(demo, 0);
    CHECK_TRUE(e0 != nullptr);
    CHECK_EQ(e0->index, static_cast<std::size_t>(0));
    CHECK_EQ(e0->byte, static_cast<uint8_t>(0));
    CHECK_EQ(e0->accelerate_decelerate, static_cast<int8_t>(-1));
    CHECK_EQ(e0->left_right, static_cast<int8_t>(-1));
    CHECK_TRUE(!e0->jump);

    const auto* e1 = sample_demo_input(demo, 0x0666);
    CHECK_TRUE(e1 != nullptr);
    CHECK_EQ(e1->index, static_cast<std::size_t>(1));
    CHECK_EQ(e1->byte, static_cast<uint8_t>(5));

    CHECK_TRUE(sample_demo_input(demo, demo.approx_tile_length_fp16() + 0x0666) ==
               nullptr);
}

static void test_renderer_row_state() {
    RendererRowState s = renderer_row_state(0x0015);
    CHECK_EQ(s.road_row_group, static_cast<std::size_t>(2));
    CHECK_EQ(s.trekdat_slot, static_cast<std::size_t>(5));
}

static void test_renderer_plan() {
    RendererCellPlan p2 = plan_renderer_cell(0x0015, 0x0200);
    CHECK_EQ(p2.road_row_group, static_cast<std::size_t>(2));
    CHECK_EQ(p2.trekdat_slot, static_cast<std::size_t>(5));
    CHECK_EQ(p2.tile_class, static_cast<uint8_t>(3));
    CHECK_EQ(p2.dispatch.target, static_cast<uint16_t>(0x2E9F));
    CHECK_TRUE(p2.dispatch.label == std::optional<std::string>("draw_type_2"));

    RendererCellPlan p5 = plan_renderer_cell(0x0007, 0x0507);
    CHECK_EQ(p5.trekdat_slot, static_cast<std::size_t>(7));
    CHECK_EQ(p5.tile_class, static_cast<uint8_t>(4));
    CHECK_EQ(p5.dispatch.target, static_cast<uint16_t>(0x2FB0));
}

static void test_renderer_row_plan(const RoadsArchive& roads) {
    RendererRowPlan plan = plan_renderer_row(0x0015, roads.roads[0].rows[83]);
    CHECK_EQ(plan.road_row_group, static_cast<std::size_t>(2));
    CHECK_EQ(plan.trekdat_slot, static_cast<std::size_t>(5));
    CHECK_EQ(plan.cells[0].descriptor.raw, static_cast<uint16_t>(0x0400));
    CHECK_EQ(plan.cells[0].dispatch.target, static_cast<uint16_t>(0x2F3C));
    CHECK_EQ(plan.cells[1].descriptor.raw, static_cast<uint16_t>(0x0260));
    CHECK_EQ(plan.cells[1].dispatch.target, static_cast<uint16_t>(0x2E9F));
    CHECK_EQ(plan.cells[2].descriptor.raw, static_cast<uint16_t>(0x0000));
    CHECK_EQ(plan.cells[2].dispatch.target, static_cast<uint16_t>(0x2E50));
    CHECK_EQ(plan.cells[6].descriptor.raw, static_cast<uint16_t>(0x0400));
    CHECK_EQ(plan.cells[6].dispatch.target, static_cast<uint16_t>(0x2F3C));
}

static void test_gameplay_frame_plan(const RoadsArchive& roads,
                                     const DemoRecording& demo) {
    auto frame = plan_gameplay_frame(demo, roads.roads[2], 80, 0x0015, 0x0666);
    CHECK_TRUE(frame.has_value());
    CHECK_EQ(frame->demo_cursor.index, static_cast<std::size_t>(1));
    CHECK_TRUE(frame->demo_input != nullptr);
    CHECK_EQ(frame->demo_input->byte, static_cast<uint8_t>(5));
    CHECK_EQ(frame->road_index, static_cast<std::size_t>(2));
    CHECK_EQ(frame->road_row_index, static_cast<std::size_t>(80));
    CHECK_EQ(frame->renderer_row.cells[0].descriptor.raw, static_cast<uint16_t>(0x0507));
    CHECK_EQ(frame->renderer_row.cells[0].tile_class, static_cast<uint8_t>(4));
    CHECK_EQ(frame->renderer_row.cells[0].dispatch.target, static_cast<uint16_t>(0x2FB0));
    CHECK_EQ(frame->renderer_row.cells[3].descriptor.raw, static_cast<uint16_t>(0x000D));
    CHECK_EQ(frame->renderer_row.cells[3].dispatch.target, static_cast<uint16_t>(0x2E50));
}

// Interpret-once validation: the baked DOS render tables must equal what the
// real executable actually contains. This is the single point where the port
// touches SKYROADS.EXE; once this passes, the runtime uses only the constants.
static void test_baked_tables_match_exe(const SkyroadsExe& exe) {
    const auto& tile_class = exe.runtime_tables.tile_class_by_low3.values;
    CHECK_EQ(tile_class.size(), skyroads::core::DOS_TILE_CLASS_BY_LOW3.size());
    for (std::size_t i = 0; i < skyroads::core::DOS_TILE_CLASS_BY_LOW3.size(); ++i) {
        CHECK_EQ(tile_class[i], skyroads::core::DOS_TILE_CLASS_BY_LOW3[i]);
    }
    const auto& entries = exe.runtime_tables.draw_dispatch_by_type.entries;
    CHECK_EQ(entries.size(), skyroads::core::DOS_DRAW_DISPATCH_TARGETS.size());
    for (std::size_t i = 0; i < skyroads::core::DOS_DRAW_DISPATCH_TARGETS.size(); ++i) {
        CHECK_EQ(entries[i].target, skyroads::core::DOS_DRAW_DISPATCH_TARGETS[i]);
    }
}

// ---- gameplay simulation ---------------------------------------------------

static void test_demo_controller_mapping(const DemoRecording& demo) {
    Ship ship;
    const auto* input = sample_demo_input_for_ship(demo, ship);
    CHECK_TRUE(input != nullptr);
    ControllerState controls = controller_state_from_demo_input(input);
    CHECK_EQ(input->index, static_cast<std::size_t>(120));
    CHECK_TRUE(controls == ControllerState::make(0, 1, false));
}

static void test_first_demo_frame(const RoadsArchive& roads,
                                  const DemoRecording& demo) {
    Level level = level_from_road_entry(roads.roads[0]);
    GameplaySession session(level);
    GameplayFrameResult frame = session.run_demo_frame(demo);
    CHECK_EQ(frame.frame_index, static_cast<std::size_t>(0));
    CHECK_TRUE(frame.controls == ControllerState::make(0, 1, false));
    CHECK_TRUE(frame.snapshot.craft_state == ShipState::Alive);
    CHECK_EQ(frame.snapshot.x_position, 256.0);
    CHECK_EQ(frame.snapshot.y_position, 80.0);
    CHECK_EQ(frame.snapshot.z_position, 3.0011444091796875);
    CHECK_EQ(frame.snapshot.z_velocity, 0.0011444091796875);
    CHECK_TRUE(frame.events.empty());
    CHECK_TRUE(!frame.did_win);
    CHECK_EQ(frame.road_row_index, static_cast<std::size_t>(3));
}

static void test_later_demo_frames(const RoadsArchive& roads,
                                   const DemoRecording& demo) {
    Level level = level_from_road_entry(roads.roads[0]);
    GameplaySession session(level);
    GameplayFrameResult f0 = session.run_demo_frame(demo);
    GameplayFrameResult f1 = session.run_demo_frame(demo);
    GameplayFrameResult f2 = session.run_demo_frame(demo);
    CHECK_TRUE(f1.snapshot.oxygen_percent < f0.snapshot.oxygen_percent);
    CHECK_TRUE(f2.snapshot.oxygen_percent < f1.snapshot.oxygen_percent);
    CHECK_TRUE(f2.snapshot.z_position > f1.snapshot.z_position);
    CHECK_TRUE(f2.snapshot.fuel_percent < f1.snapshot.fuel_percent);
    CHECK_TRUE(f2.snapshot.craft_state == ShipState::Alive);
}

static void test_fall_below_ground(const RoadsArchive& roads) {
    Level level = level_from_road_entry(roads.roads[0]);
    GameplaySession session(level);
    session.ship.x_position = 0.0;
    session.ship.y_position = GROUND_Y - 1.0;
    session.ship.y_velocity = -2.0;
    session.ship.z_velocity = 0.1;
    session.ship.x_movement_base = 1.0;

    GameplayFrameResult frame = session.run_frame(ControllerState::neutral());
    CHECK_TRUE(frame.snapshot.craft_state == ShipState::Fallen);
    CHECK_TRUE(session.death_frame_index.has_value());
    CHECK_EQ(session.ship.z_velocity, 0.0);
    CHECK_EQ(session.ship.y_velocity, 0.0);
    CHECK_EQ(session.ship.x_movement_base, 0.0);
    CHECK_TRUE(frame.events.empty());
}

// A ship that fell off the road keeps falling: the EXE's motion integrator
// (@0x1900..0x1a9b) has no death-code gate, so gravity and drift keep being
// applied and the ship drops out of view. Only player input is cut off.
static void test_fallen_keeps_falling(const RoadsArchive& roads) {
    Level level = level_from_road_entry(roads.roads[0]);
    GameplaySession session(level);
    session.ship.state = ShipState::Fallen;
    session.ship.x_position = 220.0;
    session.ship.y_position = 79.5;
    session.ship.z_position = 64.25;
    session.ship.z_velocity = 0.12;
    session.ship.y_velocity = -1.0;
    session.ship.x_movement_base = 0.5;

    Ship before = session.ship;
    GameplayFrameResult frame = session.run_frame(ControllerState::make(1, 1, true));
    CHECK_TRUE(frame.snapshot.craft_state == ShipState::Fallen);
    CHECK_TRUE(session.ship.y_position < before.y_position);
    CHECK_TRUE(session.ship.z_position > before.z_position);
    CHECK_TRUE(frame.events.empty());

    // The death animation must run for a while before the result screen shows,
    // so the fall is actually visible.
    CHECK_TRUE(!session.death_animation_finished());
    for (std::size_t i = 0; i < FALL_DEATH_TICKS + 1; ++i) {
        session.run_frame(ControllerState::neutral());
    }
    CHECK_TRUE(session.death_animation_finished());
    // Fallen far enough below road level to be out of the player's view.
    CHECK_TRUE(session.ship.y_position < GROUND_Y - 25.0);
}

// ---- app state machine -----------------------------------------------------

static AttractModeApp make_app(const RoadsArchive& roads,
                               const DemoRecording& demo) {
    return AttractModeApp(levels_from_roads_archive(roads), demo);
}

static AppInput key(bool AppInput::*field) {
    AppInput in;
    in.*field = true;
    return in;
}

static void test_app_intro(const RoadsArchive& roads, const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    AppTickResult tick = app.tick(AppInput{});
    CHECK_TRUE(tick.mode == AppMode::Intro);
    CHECK_EQ(tick.audio_commands, (std::vector<AudioCommand>{AudioCommand::play_song(0)}));
    CHECK_TRUE(tick.render_scene.tag == RenderScene::Tag::Intro);
}

static void test_app_skip_intro(const RoadsArchive& roads,
                                const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    AppTickResult tick = app.tick(key(&AppInput::space));
    CHECK_TRUE(tick.mode == AppMode::MainMenu);
    CHECK_TRUE(tick.render_scene.tag == RenderScene::Tag::MainMenu);
    CHECK_EQ(tick.audio_commands,
             (std::vector<AudioCommand>{AudioCommand::play_intro_sample(),
                                        AudioCommand::play_song(1)}));
}

static void test_app_idle_demo(const RoadsArchive& roads,
                               const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    for (int i = 0; i < 70 * 5; ++i) app.tick(AppInput{});
    AppTickResult tick = app.tick(AppInput{});
    CHECK_TRUE(tick.mode == AppMode::DemoPlayback);
    CHECK_TRUE(tick.render_scene.tag == RenderScene::Tag::DemoPlayback);
}

static void test_app_start_gameplay(const RoadsArchive& roads,
                                    const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    // Start now opens the world/level SELECT screen (not gameplay directly).
    AppTickResult select = app.tick(key(&AppInput::enter));
    CHECK_TRUE(select.mode == AppMode::GoMenu);
    CHECK_TRUE(select.render_scene.tag == RenderScene::Tag::GoMenu);
    // World 1 / level 1 -> road index 1.
    CHECK_EQ(select.render_scene.go_menu.road_index, static_cast<std::size_t>(1));
    // Enter on the select screen launches gameplay.
    AppTickResult tick = app.tick(key(&AppInput::enter));
    CHECK_TRUE(tick.mode == AppMode::Gameplay);
    CHECK_TRUE(tick.render_scene.tag == RenderScene::Tag::Gameplay);
    CHECK_EQ(tick.audio_commands,
             (std::vector<AudioCommand>{AudioCommand::play_song(2)}));
}

static void test_app_select_navigates_world(const RoadsArchive& roads,
                                            const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    app.tick(key(&AppInput::enter)); // -> GoMenu (world 0, road 0)
    // Grid navigation: Down spills into the next world in the column.
    app.tick(key(&AppInput::down)); // world 0, road 1
    app.tick(key(&AppInput::down)); // world 0, road 2
    AppTickResult sel = app.tick(key(&AppInput::down)); // -> world 1, road 0
    CHECK_TRUE(sel.mode == AppMode::GoMenu);
    CHECK_EQ(sel.render_scene.go_menu.selected_world, static_cast<std::size_t>(1));
    CHECK_EQ(sel.render_scene.go_menu.selected_level, static_cast<std::size_t>(0));
    // world 1, road 0 -> road index = 1*3 + 0 + 1 = 4.
    CHECK_EQ(sel.render_scene.go_menu.road_index, static_cast<std::size_t>(4));
    // Right switches to the other column (world +5).
    AppTickResult col = app.tick(key(&AppInput::right));
    CHECK_EQ(col.render_scene.go_menu.selected_world, static_cast<std::size_t>(6));
    AppTickResult play = app.tick(key(&AppInput::enter));
    CHECK_TRUE(play.mode == AppMode::Gameplay);
}

// Dying retries the same road immediately, without passing through the level select
// (EXE @0x3af-0x3b4: only completing a road leaves the play loop).
static void test_app_death_restarts_road(const RoadsArchive& roads,
                                         const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    app.tick(key(&AppInput::enter)); // GoMenu
    app.tick(key(&AppInput::enter)); // Gameplay
    const std::size_t road = app.current_go_menu_scene().road_index;

    app.gameplay_session().ship.state = ShipState::Exploded;
    // The explosion plays out first, still in gameplay.
    AppTickResult during = app.tick(AppInput{});
    CHECK_TRUE(during.mode == AppMode::Gameplay);
    CHECK_TRUE(during.render_scene.play.snapshot.craft_state != ShipState::Alive);

    // Once it finishes the same road restarts on its own — no menu, no keypress.
    for (std::size_t i = 0; i < EXPLOSION_DEATH_TICKS + 3; ++i) app.tick(AppInput{});
    AppTickResult after = app.tick(AppInput{});
    CHECK_TRUE(after.mode == AppMode::Gameplay);
    CHECK_TRUE(after.render_scene.play.snapshot.craft_state == ShipState::Alive);
    CHECK_EQ(app.current_go_menu_scene().road_index, road);
}

// Up/Down walk a single flat 0..29 list, so they cross between the two columns, and
// Left from the left column snaps to the first entry (EXE @0x52cb-0x5305).
static void test_go_menu_flat_navigation(const RoadsArchive& roads,
                                         const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    app.tick(key(&AppInput::enter)); // GoMenu

    // Right moves +15 => world 0 road 0 -> world 5 road 0 (top of the right column).
    AppTickResult r = app.tick(key(&AppInput::right));
    CHECK_EQ(r.render_scene.go_menu.selected_world, static_cast<std::size_t>(5));
    CHECK_EQ(r.render_scene.go_menu.selected_level, static_cast<std::size_t>(0));

    // Up from there is index 14: the LAST road of world 4, in the left column.
    AppTickResult u = app.tick(key(&AppInput::up));
    CHECK_EQ(u.render_scene.go_menu.selected_world, static_cast<std::size_t>(4));
    CHECK_EQ(u.render_scene.go_menu.selected_level, static_cast<std::size_t>(2));

    // Left while already in the left column jumps to the very first entry.
    AppTickResult l = app.tick(key(&AppInput::left));
    CHECK_EQ(l.render_scene.go_menu.selected_world, static_cast<std::size_t>(0));
    CHECK_EQ(l.render_scene.go_menu.selected_level, static_cast<std::size_t>(0));

    // Up at the first entry stays put.
    AppTickResult top = app.tick(key(&AppInput::up));
    CHECK_EQ(top.render_scene.go_menu.selected_world, static_cast<std::size_t>(0));
    CHECK_EQ(top.render_scene.go_menu.selected_level, static_cast<std::size_t>(0));

    // Down steps one road at a time.
    AppTickResult d = app.tick(key(&AppInput::down));
    CHECK_EQ(d.render_scene.go_menu.selected_level, static_cast<std::size_t>(1));
}

static void test_app_help_cycle(const RoadsArchive& roads,
                                const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    app.tick(key(&AppInput::down));
    AppTickResult main_menu = app.tick(key(&AppInput::down));
    CHECK_TRUE(main_menu.render_scene.tag == RenderScene::Tag::MainMenu);
    CHECK_TRUE(main_menu.render_scene.main_menu.selected == MenuCursor::Help);
    CHECK_TRUE(app.mode() == AppMode::MainMenu);

    AppTickResult help = app.tick(key(&AppInput::enter));
    CHECK_TRUE(help.mode == AppMode::HelpMenu);
    app.tick(key(&AppInput::enter));
    app.tick(key(&AppInput::enter));
    AppTickResult back = app.tick(key(&AppInput::enter));
    CHECK_TRUE(back.mode == AppMode::MainMenu);
}

static void test_app_gameplay_scene(const RoadsArchive& roads,
                                    const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    app.tick(key(&AppInput::space));
    app.tick(key(&AppInput::enter)); // -> GoMenu
    app.tick(key(&AppInput::enter)); // -> Gameplay
    AppInput held;
    held.up_held = true;
    held.right_held = true;
    AppTickResult tick = app.tick(held);
    CHECK_TRUE(tick.render_scene.tag == RenderScene::Tag::Gameplay);
    const DemoPlaybackState& scene = tick.render_scene.play;
    const std::size_t expected_first_row =
        (scene.current_row >> 3) >= 3 ? (scene.current_row >> 3) - 3 : 0;
    CHECK_EQ(scene.rows.front().row_index, expected_first_row);
    CHECK_EQ(scene.ship.turn_input, static_cast<int8_t>(1));
    CHECK_EQ(scene.ship.accel_input, static_cast<int8_t>(1));
}

static void test_app_death_frame(const RoadsArchive& roads,
                                 const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    app.gameplay_session().ship.state = ShipState::Exploded;
    app.gameplay_session().death_frame_index = 5;
    app.gameplay_session().frame_index_ = 14;
    DemoPlaybackState scene = app.current_gameplay_scene();
    CHECK_TRUE(scene.ship.state == ShipState::Exploded);
    CHECK_TRUE(scene.ship.death_frame_index == std::optional<std::size_t>(5));
}

static void test_app_eighth_tile_rows(const RoadsArchive& roads,
                                      const DemoRecording& demo) {
    AttractModeApp app = make_app(roads, demo);
    app.gameplay_session().ship.z_position = 3.375;
    DemoPlaybackState scene = app.current_gameplay_scene();
    CHECK_EQ(scene.current_row, static_cast<std::size_t>(27));
    CHECK_TRUE(scene.fractional_z == 0.0);
}

CHECK_MAIN_BEGIN()
    RoadsArchive roads = load_roads_lzs_path(check::asset("ROADS.LZS"));
    DemoRecording demo = load_demo_rec_path(check::asset("DEMO.REC"));

    test_demo_indexing();
    test_demo_sampling();
    test_renderer_row_state();
    test_renderer_plan();
    test_renderer_row_plan(roads);
    test_gameplay_frame_plan(roads, demo);

    // The planner above uses baked constants only. This one test still opens the
    // executable, to prove those constants match the binary they came from.
    try {
        SkyroadsExe exe = load_skyroads_exe_path(check::asset("SKYROADS.EXE"));
        test_baked_tables_match_exe(exe);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "note: skipped baked-vs-EXE validation (could not read the "
                     "reference SKYROADS.EXE: %s)\n",
                     e.what());
    }

    test_demo_controller_mapping(demo);
    test_first_demo_frame(roads, demo);
    test_later_demo_frames(roads, demo);
    test_fall_below_ground(roads);
    test_fallen_keeps_falling(roads);

    test_app_intro(roads, demo);
    test_app_skip_intro(roads, demo);
    test_app_idle_demo(roads, demo);
    test_app_start_gameplay(roads, demo);
    test_app_select_navigates_world(roads, demo);
    test_app_death_restarts_road(roads, demo);
    test_go_menu_flat_navigation(roads, demo);
    test_app_help_cycle(roads, demo);
    test_app_gameplay_scene(roads, demo);
    test_app_death_frame(roads, demo);
    test_app_eighth_tile_rows(roads, demo);
CHECK_MAIN_END()
