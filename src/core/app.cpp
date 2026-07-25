#include "core/app.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace skyroads::core {
namespace {

constexpr std::size_t TICKS_PER_SECOND = 70;
constexpr std::size_t INTRO_SOUND_DELAY_TICKS = TICKS_PER_SECOND / 2;
constexpr std::size_t INTRO_ANIM_START_TICKS = TICKS_PER_SECOND * 2;
constexpr std::size_t INTRO_TITLE_HOLD_TICKS = TICKS_PER_SECOND * 4;
constexpr std::size_t CREDIT_FRAME_TICKS = TICKS_PER_SECOND * 4;
constexpr std::size_t MENU_IDLE_DEMO_TICKS = TICKS_PER_SECOND * 5;
constexpr std::size_t RENDER_ROWS_BEHIND = 3;
constexpr std::size_t RENDER_ROWS_AHEAD = 7;
constexpr uint8_t MENU_SONG_INDEX = 1;
constexpr uint8_t GAMEPLAY_SONG_INDEX = 2;
constexpr uint8_t DEMO_SONG_INDEX = 2;

MenuCursor menu_cursor_move_by(MenuCursor cursor, int delta) {
    const int idx = static_cast<int>(menu_cursor_index(cursor)) + delta;
    const int clamped = std::clamp(idx, 0, 2);
    switch (clamped) {
        case 0: return MenuCursor::Start;
        case 1: return MenuCursor::Config;
        default: return MenuCursor::Help;
    }
}

std::size_t world_index_for_level(std::size_t level_index) {
    return level_index == 0 ? 0 : (level_index - 1) / 3;
}

int8_t axis(bool negative, bool positive) {
    if (negative && !positive) return -1;
    if (!negative && positive) return 1;
    return 0;
}

void emit_sfx_for_events(const std::vector<GameplayEvent>& events,
                         std::vector<AudioCommand>& audio) {
    for (GameplayEvent event : events) {
        std::optional<uint8_t> sfx;
        switch (event) {
            // SFX indices verified from the executable (player @0x3c2): bump=0,
            // bounce=1 (the heavy machine thud), explode=2, refill=4. The port
            // previously used bounce=3 (a short beep) and explode=1.
            case GameplayEvent::ShipBumpedWall: sfx = 0; break;
            case GameplayEvent::ShipExploded: sfx = 2; break;
            case GameplayEvent::ShipBounced: sfx = 1; break;
            case GameplayEvent::ShipRefilled: sfx = 4; break;
        }
        if (sfx) {
            audio.push_back(AudioCommand::play_sfx(*sfx));
        }
    }
}

ShipRenderState build_ship_render_state(const GameplaySession& session) {
    ShipRenderState s;
    s.x_position = session.ship.x_position;
    s.y_position = session.ship.y_position;
    s.z_position = session.ship.z_position;
    s.y_velocity = session.ship.y_velocity;
    s.z_velocity =
        session.ship.z_velocity + session.ship.jump_o_master_velocity_delta;
    s.state = session.ship.state;
    s.is_on_ground = session.ship.is_on_ground;
    s.is_going_up = session.ship.is_going_up;
    // Resting on a surface: that surface is right under us. Airborne: the surface we
    // left. Either way the shadow lands on solid ground rather than a fixed row.
    s.support_y = session.ship.is_on_ground ? session.ship.y_position
                                            : session.ship.jumped_from_y_position;
    s.turn_input = session.last_controls.turn_input;
    s.accel_input = session.last_controls.accel_input;
    s.jump_input = session.last_controls.jump_input;
    s.death_frame_index = session.death_frame_index;
    return s;
}

} // namespace

std::size_t menu_cursor_index(MenuCursor cursor) {
    switch (cursor) {
        case MenuCursor::Start: return 0;
        case MenuCursor::Config: return 1;
        case MenuCursor::Help: return 2;
    }
    return 0;
}

ControllerState AppInput::gameplay_controls() const {
    return ControllerState::make(axis(left_held, right_held),
                                 axis(down_held, up_held),
                                 enter_held || space_held);
}

AttractModeApp::AttractModeApp(std::vector<skyroads::data::Level> levels,
                               skyroads::data::DemoRecording demo_recording)
    : levels_(std::move(levels)),
      mode_(AppMode::Intro),
      current_level_index_(0),
      demo_level_index_(0),
      demo_recording_(std::move(demo_recording)),
      demo_session_(levels_.at(0)),
      gameplay_session_(levels_.at(0)),
      intro_tick_(0),
      menu_idle_tick_(0),
      main_menu_cursor_(MenuCursor::Start),
      help_page_(0),
      selected_world_(0),
      selected_level_(0),
      was_gameover_(false),
      awaiting_advance_release_(false),
      intro_song_started_(false),
      intro_sample_started_(false),
      menu_song_started_(false) {
    assert(!levels_.empty() && "AttractModeApp requires at least one level");
}

std::size_t AttractModeApp::world_count() const {
    // Road 0 is the demo level; roads 1.. are 3 levels per world.
    const std::size_t playable = levels_.size() > 1 ? levels_.size() - 1 : 0;
    const std::size_t worlds = (playable + 2) / 3; // ceil
    return worlds == 0 ? 1 : worlds;
}

std::size_t AttractModeApp::selected_road_index() const {
    const std::size_t road = selected_world_ * 3 + selected_level_ + 1;
    return road < levels_.size() ? road : levels_.size() - 1;
}

AppTickResult AttractModeApp::tick(AppInput input) {
    std::vector<AudioCommand> audio;
    switch (mode_) {
        case AppMode::Intro: tick_intro(input, audio); break;
        case AppMode::MainMenu: tick_main_menu(input, audio); break;
        case AppMode::HelpMenu: tick_help_menu(input, audio); break;
        case AppMode::SettingsMenu: tick_settings_menu(input); break;
        case AppMode::GoMenu: tick_go_menu(input, audio); break;
        case AppMode::DemoPlayback: tick_demo(input, audio); break;
        case AppMode::Gameplay: tick_gameplay(input, audio); break;
        case AppMode::Boot:
            mode_ = AppMode::MainMenu;
            break;
    }

    AppTickResult result;
    result.mode = mode_;
    result.render_scene = current_render_scene();
    result.audio_commands = std::move(audio);
    return result;
}

void AttractModeApp::tick_intro(AppInput input, std::vector<AudioCommand>& audio) {
    if (!intro_song_started_) {
        audio.push_back(AudioCommand::play_song(0));
        intro_song_started_ = true;
    }
    if (!intro_sample_started_ && intro_tick_ >= INTRO_SOUND_DELAY_TICKS) {
        audio.push_back(AudioCommand::play_intro_sample());
        intro_sample_started_ = true;
    }
    if (intro_tick_ >= INTRO_SOUND_DELAY_TICKS && input.skip_requested()) {
        enter_main_menu(audio);
        return;
    }

    if (intro_tick_ >= final_credit_end_tick()) {
        enter_main_menu(audio);
        return;
    }

    intro_tick_ += 1;
}

void AttractModeApp::tick_main_menu(AppInput input,
                                    std::vector<AudioCommand>& audio) {
    if (!menu_song_started_) {
        audio.push_back(AudioCommand::play_song(MENU_SONG_INDEX));
        menu_song_started_ = true;
    }

    bool navigated = false;
    if (input.up) {
        main_menu_cursor_ = menu_cursor_move_by(main_menu_cursor_, -1);
        navigated = true;
    }
    if (input.down) {
        main_menu_cursor_ = menu_cursor_move_by(main_menu_cursor_, 1);
        navigated = true;
    }

    if (input.enter) {
        menu_idle_tick_ = 0;
        switch (main_menu_cursor_) {
            case MenuCursor::Start:
                // Original SkyRoads flow: Start opens the world/level select
                // screen; the player picks a world before launching.
                selected_world_ = 0;
                selected_level_ = 0;
                enter_select(audio, false);
                break;
            case MenuCursor::Config: mode_ = AppMode::SettingsMenu; break;
            case MenuCursor::Help:
                help_page_ = 0;
                mode_ = AppMode::HelpMenu;
                break;
        }
        return;
    }

    if (navigated || input.escape || input.space) {
        menu_idle_tick_ = 0;
    } else {
        menu_idle_tick_ += 1;
    }

    if (menu_idle_tick_ >= MENU_IDLE_DEMO_TICKS) {
        start_demo(audio);
    }
}

void AttractModeApp::tick_help_menu(AppInput input,
                                    std::vector<AudioCommand>& /*audio*/) {
    if (input.escape) {
        mode_ = AppMode::MainMenu;
        menu_idle_tick_ = 0;
        return;
    }
    if (input.enter || input.space) {
        help_page_ += 1;
        if (help_page_ >= 3) {
            help_page_ = 0;
            mode_ = AppMode::MainMenu;
        }
        menu_idle_tick_ = 0;
    }
}

void AttractModeApp::tick_settings_menu(AppInput input) {
    if (input.escape || input.enter || input.space) {
        mode_ = AppMode::MainMenu;
        menu_idle_tick_ = 0;
    }
}

void AttractModeApp::tick_go_menu(AppInput input, std::vector<AudioCommand>& audio) {
    if (input.escape) {
        enter_main_menu(audio);
        return;
    }
    // The GOMENU grid is two columns (worlds 0-4 left, 5-9 right), each world a
    // row of 3 roads. Left/Right switch column; Up/Down move road, spilling into
    // the adjacent world in the same column.
    const std::size_t worlds = world_count();
    const std::size_t row = selected_world_ % 5;
    if (input.left && selected_world_ >= 5) selected_world_ -= 5;
    if (input.right && selected_world_ + 5 < worlds) selected_world_ += 5;
    if (input.up) {
        if (selected_level_ > 0) {
            selected_level_ -= 1;
        } else if (row > 0) {
            selected_world_ -= 1; // previous world in this column
            selected_level_ = 2;
        }
    }
    if (input.down) {
        if (selected_level_ < 2) {
            selected_level_ += 1;
        } else if (row < 4 && selected_world_ + 1 < worlds) {
            selected_world_ += 1; // next world in this column
            selected_level_ = 0;
        }
    }

    if (input.enter) {
        current_level_index_ = selected_road_index();
        start_gameplay(audio);
    }
}

void AttractModeApp::enter_select(std::vector<AudioCommand>& audio,
                                  bool switch_song) {
    mode_ = AppMode::GoMenu;
    menu_idle_tick_ = 0;
    if (switch_song) {
        audio.push_back(AudioCommand::play_song(MENU_SONG_INDEX));
    }
}

void AttractModeApp::tick_demo(AppInput input, std::vector<AudioCommand>& audio) {
    if (input.escape || input.enter || input.space) {
        return_to_menu(audio);
        return;
    }
    if (sample_demo_input_for_ship(demo_recording_, demo_session_.ship) ==
        nullptr) {
        return_to_menu(audio);
        return;
    }
    demo_session_.run_demo_frame(demo_recording_);
}

void AttractModeApp::tick_gameplay(AppInput input,
                                   std::vector<AudioCommand>& audio) {
    if (input.escape) {
        enter_select(audio, true); // Esc -> back to level select
        return;
    }

    // While the ship is dying, keep simulating so the death plays out on screen:
    // a crash runs its explosion animation, and a ship that fell off the road
    // keeps falling out of view. Only then show the result.
    const bool dying = gameplay_session_.ship.state != ShipState::Alive &&
                       !gameplay_session_.death_animation_finished();
    if (dying && !gameplay_session_.did_win) {
        GameplayFrameResult result =
            gameplay_session_.run_frame(input.gameplay_controls());
        emit_sfx_for_events(result.events, audio);
        return;
    }

    const bool over = gameplay_session_.did_win ||
                      gameplay_session_.ship.state != ShipState::Alive;
    if (over) {
        // Latch on the first over-frame and require the throttle/jump keys to be
        // released before a press counts — otherwise a held Space/Enter from
        // gameplay would instantly skip the result screen (the "auto-loop" bug).
        if (!was_gameover_) {
            was_gameover_ = true;
            awaiting_advance_release_ = true;
        }
        if (awaiting_advance_release_) {
            if (!input.enter_held && !input.space_held) {
                awaiting_advance_release_ = false;
            }
            return;
        }
        if (input.enter) {
            if (gameplay_session_.did_win && selected_level_ < 2) {
                selected_level_ += 1; // advance within the world
                current_level_index_ = selected_road_index();
                start_gameplay(audio);
            } else {
                // Crashed, or finished the world -> back to level select.
                enter_select(audio, true);
            }
        }
        return;
    }

    was_gameover_ = false;
    GameplayFrameResult result =
        gameplay_session_.run_frame(input.gameplay_controls());
    emit_sfx_for_events(result.events, audio);
}

void AttractModeApp::start_demo(std::vector<AudioCommand>& audio) {
    mode_ = AppMode::DemoPlayback;
    menu_idle_tick_ = 0;
    demo_session_ = GameplaySession(levels_[demo_level_index_]);
    audio.push_back(AudioCommand::play_song(DEMO_SONG_INDEX));
}

void AttractModeApp::start_gameplay(std::vector<AudioCommand>& audio) {
    mode_ = AppMode::Gameplay;
    menu_idle_tick_ = 0;
    was_gameover_ = false;
    awaiting_advance_release_ = false;
    gameplay_session_ = GameplaySession(levels_[current_level_index_]);
    audio.push_back(AudioCommand::play_song(GAMEPLAY_SONG_INDEX));
}

void AttractModeApp::enter_main_menu(std::vector<AudioCommand>& audio) {
    mode_ = AppMode::MainMenu;
    menu_idle_tick_ = 0;
    main_menu_cursor_ = MenuCursor::Start;
    menu_song_started_ = false;
    audio.push_back(AudioCommand::play_song(MENU_SONG_INDEX));
    menu_song_started_ = true;
}

void AttractModeApp::return_to_menu(std::vector<AudioCommand>& audio) {
    mode_ = AppMode::MainMenu;
    menu_idle_tick_ = 0;
    main_menu_cursor_ = MenuCursor::Start;
    menu_song_started_ = false;
    audio.push_back(AudioCommand::play_song(MENU_SONG_INDEX));
    menu_song_started_ = true;
}

RenderScene AttractModeApp::current_render_scene() const {
    RenderScene scene;
    switch (mode_) {
        case AppMode::Intro:
            scene.tag = RenderScene::Tag::Intro;
            scene.intro = current_intro_scene();
            break;
        case AppMode::MainMenu:
            scene.tag = RenderScene::Tag::MainMenu;
            scene.main_menu = MainMenuScene{main_menu_cursor_};
            break;
        case AppMode::HelpMenu:
            scene.tag = RenderScene::Tag::HelpMenu;
            scene.help_menu = HelpMenuScene{help_page_};
            break;
        case AppMode::SettingsMenu:
            scene.tag = RenderScene::Tag::SettingsMenu;
            scene.settings_menu = SettingsMenuScene{0};
            break;
        case AppMode::GoMenu:
            scene.tag = RenderScene::Tag::GoMenu;
            scene.go_menu = current_go_menu_scene();
            break;
        case AppMode::DemoPlayback:
            scene.tag = RenderScene::Tag::DemoPlayback;
            scene.play = current_demo_scene();
            break;
        case AppMode::Gameplay:
            scene.tag = RenderScene::Tag::Gameplay;
            scene.play = current_gameplay_scene();
            break;
        case AppMode::Boot:
            scene.tag = RenderScene::Tag::MainMenu;
            scene.main_menu = MainMenuScene{main_menu_cursor_};
            break;
    }
    return scene;
}

GoMenuScene AttractModeApp::current_go_menu_scene() const {
    const std::size_t road = selected_road_index();
    const skyroads::data::Level& level = levels_[road];
    GoMenuScene scene;
    scene.selected_world = selected_world_;
    scene.selected_level = selected_level_;
    scene.world_count = world_count();
    scene.road_index = road;
    scene.gravity = level.gravity;
    scene.fuel = level.fuel;
    scene.oxygen = level.oxygen;
    return scene;
}

IntroSequenceState AttractModeApp::current_intro_scene() const {
    const std::size_t anim_frame_count = 100;
    const std::size_t credit_frame_count = 8;
    const std::size_t title_start = INTRO_ANIM_START_TICKS + anim_frame_count;
    const std::size_t credits_start = title_start + INTRO_TITLE_HOLD_TICKS;

    const float background_brightness = std::min(
        static_cast<float>(intro_tick_) / static_cast<float>(TICKS_PER_SECOND),
        1.0f);

    std::optional<std::size_t> anim_frame_index;
    if (intro_tick_ >= INTRO_ANIM_START_TICKS) {
        const std::size_t index = intro_tick_ - INTRO_ANIM_START_TICKS;
        if (index < anim_frame_count) {
            anim_frame_index = index;
        }
    }

    float title_progress = 0.0f;
    if (intro_tick_ >= title_start) {
        const std::size_t ticks = intro_tick_ - title_start;
        title_progress = std::min(static_cast<float>(ticks) /
                                      (static_cast<float>(TICKS_PER_SECOND) * 3.5f),
                                  1.0f);
    }

    const std::size_t credit_ticks =
        intro_tick_ >= credits_start ? intro_tick_ - credits_start : 0;
    std::optional<std::size_t> credit_frame_index;
    if (intro_tick_ >= credits_start) {
        credit_frame_index =
            std::min(credit_ticks / CREDIT_FRAME_TICKS, credit_frame_count - 1);
    }

    float credit_alpha;
    if (intro_tick_ < credits_start) {
        credit_alpha = 0.0f;
    } else {
        const std::size_t seq = credit_ticks % CREDIT_FRAME_TICKS;
        if (seq < TICKS_PER_SECOND) {
            credit_alpha =
                static_cast<float>(seq) / static_cast<float>(TICKS_PER_SECOND);
        } else if (seq > TICKS_PER_SECOND * 3) {
            credit_alpha = static_cast<float>(CREDIT_FRAME_TICKS - seq) /
                           static_cast<float>(TICKS_PER_SECOND);
        } else {
            credit_alpha = 1.0f;
        }
    }

    return IntroSequenceState{intro_tick_,      background_brightness,
                              title_progress,   anim_frame_index,
                              credit_frame_index, credit_alpha};
}

DemoPlaybackState AttractModeApp::current_demo_scene() const {
    return build_play_scene(demo_session_, true);
}

DemoPlaybackState AttractModeApp::current_gameplay_scene() const {
    return build_play_scene(gameplay_session_, false);
}

DemoPlaybackState AttractModeApp::build_play_scene(const GameplaySession& session,
                                                  bool is_demo) const {
    const std::size_t current_row = static_cast<std::size_t>(
        std::max(std::floor(session.ship.z_position * 8.0), 0.0));
    const std::size_t current_group = current_row >> 3;
    const std::size_t start_row =
        current_group >= RENDER_ROWS_BEHIND ? current_group - RENDER_ROWS_BEHIND
                                            : 0;
    const std::size_t end_row =
        std::min(current_group + RENDER_ROWS_AHEAD + 1, session.level.length());

    std::vector<RoadRenderRow> rows;
    for (std::size_t row_index = start_row; row_index < end_row; ++row_index) {
        if (row_index < session.level.cells.size()) {
            RoadRenderRow row;
            row.row_index = row_index;
            row.cells = session.level.cells[row_index];
            rows.push_back(std::move(row));
        }
    }

    DemoPlaybackState state;
    state.world_index = world_index_for_level(session.level.road_index);
    state.gravity = session.level.gravity;
    state.level_length = session.level.length();
    state.frame_index = session.frame_index();
    state.current_row = current_row;
    state.fractional_z =
        session.ship.z_position - (static_cast<double>(current_row) / 8.0);
    state.rows = std::move(rows);
    state.did_win = session.did_win;
    state.is_demo = is_demo;
    state.death_animation_finished = session.death_animation_finished();
    state.craft_state = session.ship.state;
    state.snapshot = GameSnapshot{
        session.ship.x_position,
        session.ship.y_position,
        session.ship.z_position,
        session.ship.z_velocity + session.ship.jump_o_master_velocity_delta,
        session.ship.state,
        session.ship.oxygen_remaining / 0x7530,
        session.ship.fuel_remaining / 0x7530,
        session.ship.jump_o_master_in_use,
        session.ship.jump_o_master_velocity_delta};
    state.ship = build_ship_render_state(session);
    state.road_palette = session.level.palette;
    return state;
}

std::size_t AttractModeApp::final_credit_end_tick() const {
    const std::size_t anim_frame_count = 100;
    const std::size_t credit_frame_count = 8;
    return INTRO_ANIM_START_TICKS + anim_frame_count + INTRO_TITLE_HOLD_TICKS +
           CREDIT_FRAME_TICKS * credit_frame_count;
}

} // namespace skyroads::core
