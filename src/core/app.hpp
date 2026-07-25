// Part of the SkyRoads SDL port
//
// The attract-mode state machine: intro -> menu -> gameplay/demo, plus the
// audio-command emission and render-scene construction. `RenderScene` is a
// payload-carrying the reference design enum; here it is a tagged struct holding the possible
// scene payloads (DemoPlayback and Gameplay share DemoPlaybackState).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/gameplay.hpp"
#include "data/demo.hpp"
#include "data/level.hpp"
#include "data/roads.hpp"

namespace skyroads::core {

using skyroads::data::LevelCell;
using skyroads::data::ROAD_COLUMNS;

enum class AppMode {
    Boot,
    Intro,
    MainMenu,
    HelpMenu,
    SettingsMenu,
    GoMenu,
    DemoPlayback,
    Gameplay,
};

enum class MenuCursor {
    Start,
    Config,
    Help,
};

std::size_t menu_cursor_index(MenuCursor cursor);

struct AppInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool enter = false;
    bool escape = false;
    bool space = false;
    bool up_held = false;
    bool down_held = false;
    bool left_held = false;
    bool right_held = false;
    bool enter_held = false;
    bool space_held = false;

    bool skip_requested() const { return enter || space || escape; }
    ControllerState gameplay_controls() const;
};

enum class AudioCommandKind {
    PlaySong,
    StopSong,
    PlayIntroSample,
    PlaySfx,
    StopAllSamples,
};

struct AudioCommand {
    AudioCommandKind kind;
    uint8_t value = 0; // song/sfx index when relevant

    static AudioCommand play_song(uint8_t s) { return {AudioCommandKind::PlaySong, s}; }
    static AudioCommand stop_song() { return {AudioCommandKind::StopSong, 0}; }
    static AudioCommand play_intro_sample() {
        return {AudioCommandKind::PlayIntroSample, 0};
    }
    static AudioCommand play_sfx(uint8_t s) { return {AudioCommandKind::PlaySfx, s}; }
    static AudioCommand stop_all_samples() {
        return {AudioCommandKind::StopAllSamples, 0};
    }
    bool operator==(const AudioCommand& o) const {
        return kind == o.kind && value == o.value;
    }
};

struct IntroSequenceState {
    std::size_t tick;
    float background_brightness;
    float title_progress;
    std::optional<std::size_t> anim_frame_index;
    std::optional<std::size_t> credit_frame_index;
    float credit_alpha;
};

struct ShipRenderState {
    double x_position;
    double y_position;
    double z_position;
    double y_velocity;
    double z_velocity;
    ShipState state;
    bool is_on_ground;
    bool is_going_up;
    // Height of the surface under the ship (road level, or a raised block's top).
    double support_y;
    int8_t turn_input;
    int8_t accel_input;
    bool jump_input;
    std::optional<std::size_t> death_frame_index;
};

struct RoadRenderRow {
    std::size_t row_index;
    std::array<LevelCell, ROAD_COLUMNS> cells;
};

struct DemoPlaybackState {
    std::size_t world_index;
    uint16_t gravity;
    std::size_t level_length;
    std::size_t frame_index;
    std::size_t current_row;
    double fractional_z;
    std::vector<RoadRenderRow> rows;
    bool did_win;
    bool is_demo;
    // Result text waits on this so it never covers the explosion / the ship falling.
    bool death_animation_finished;
    ShipState craft_state;
    GameSnapshot snapshot;
    ShipRenderState ship;
    std::vector<skyroads::data::RgbColor> road_palette; // this road's VGA palette
};

struct MainMenuScene {
    MenuCursor selected;
};
struct HelpMenuScene {
    std::size_t page_index;
};
struct SettingsMenuScene {
    std::size_t frame_index;
};

// World/level select shown before gameplay (uses GOMENU art + a text overlay).
struct GoMenuScene {
    std::size_t selected_world; // 0..9
    std::size_t selected_level; // 0..2 within the world
    std::size_t world_count;
    std::size_t road_index; // resolved road (1..30)
    uint16_t gravity;
    uint16_t fuel;
    uint16_t oxygen;
};

struct RenderScene {
    enum class Tag {
        Intro,
        MainMenu,
        HelpMenu,
        SettingsMenu,
        GoMenu,
        DemoPlayback,
        Gameplay,
    } tag;
    IntroSequenceState intro{};
    MainMenuScene main_menu{};
    HelpMenuScene help_menu{};
    SettingsMenuScene settings_menu{};
    GoMenuScene go_menu{};
    DemoPlaybackState play{}; // DemoPlayback or Gameplay payload
};

struct AppTickResult {
    AppMode mode;
    RenderScene render_scene;
    std::vector<AudioCommand> audio_commands;
};

class AttractModeApp {
public:
    AttractModeApp(std::vector<skyroads::data::Level> levels,
                   skyroads::data::DemoRecording demo_recording);

    AppMode mode() const { return mode_; }
    AppTickResult tick(AppInput input);

    // Exposed for host/tests (the reference design tests reach into these fields directly).
    GameplaySession& gameplay_session() { return gameplay_session_; }
    GameplaySession& demo_session() { return demo_session_; }
    DemoPlaybackState current_gameplay_scene() const;
    DemoPlaybackState current_demo_scene() const;
    GoMenuScene current_go_menu_scene() const;

private:
    void tick_intro(AppInput input, std::vector<AudioCommand>& audio);
    void tick_main_menu(AppInput input, std::vector<AudioCommand>& audio);
    void tick_help_menu(AppInput input, std::vector<AudioCommand>& audio);
    void tick_settings_menu(AppInput input);
    void tick_go_menu(AppInput input, std::vector<AudioCommand>& audio);
    void tick_demo(AppInput input, std::vector<AudioCommand>& audio);
    void tick_gameplay(AppInput input, std::vector<AudioCommand>& audio);
    void start_demo(std::vector<AudioCommand>& audio);
    void start_gameplay(std::vector<AudioCommand>& audio);
    void enter_select(std::vector<AudioCommand>& audio, bool switch_song);
    void enter_main_menu(std::vector<AudioCommand>& audio);
    void return_to_menu(std::vector<AudioCommand>& audio);
    std::size_t world_count() const;
    std::size_t selected_road_index() const;
    RenderScene current_render_scene() const;
    IntroSequenceState current_intro_scene() const;
    DemoPlaybackState build_play_scene(const GameplaySession& session,
                                       bool is_demo) const;
    std::size_t final_credit_end_tick() const;

    std::vector<skyroads::data::Level> levels_;
    AppMode mode_;
    std::size_t current_level_index_;
    std::size_t demo_level_index_;
    skyroads::data::DemoRecording demo_recording_;
    GameplaySession demo_session_;
    GameplaySession gameplay_session_;
    std::size_t intro_tick_;
    std::size_t menu_idle_tick_;
    MenuCursor main_menu_cursor_;
    std::size_t help_page_;
    std::size_t selected_world_;
    std::size_t selected_level_;
    bool was_gameover_;
    bool awaiting_advance_release_;
    bool intro_song_started_;
    bool intro_sample_started_;
    bool menu_song_started_;
};

} // namespace skyroads::core
