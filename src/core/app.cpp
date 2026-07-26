#include "core/app.hpp"

#include "core/dos_render_tables.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace skyroads::core {
namespace {

// The game's tick is the timer ISR's: PIT divisor 0x19E4 = 180.02 Hz, and the tick
// counter ds:0x160c is bumped on 2 of every 10 interrupts (@0x3b06-0x3b14) = 36 Hz.
constexpr std::size_t TICKS_PER_SECOND = 36;

// ---- intro sequence (EXE routine @0x4575) ----------------------------------
// Every value below is a literal tick count out of the disassembly. The helpers the
// intro uses all measure in game ticks: 0x443d waits N of them, 0x4315 spreads a
// palette fade over N of them, and both bail out the instant ds:0x54ac (the key
// flag) is set -- which is why one keypress runs the whole remainder out at once.
// Order: the background fades up -> pause -> intro.snd -> pause -> the anim.lzs
// animation -> pause -> the title wipes in -> the title flashes white and settles
// -> five credit screens -> everything fades out and the main menu takes over.
constexpr std::size_t INTRO_BG_FADE_TICKS = 36;       // 0x4b72(pal, 1, 0x24) @0x4749
constexpr std::size_t INTRO_PRE_SOUND_TICKS = 24;     // 0x443d(0x18)         @0x4756
constexpr std::size_t INTRO_POST_SOUND_TICKS = 37;    // 0x443d(0x25)         @0x477b
constexpr std::size_t INTRO_ANIM_GROUP_TICKS = 2;     // ds:0x160c >= 2       @0x47b1
constexpr std::size_t INTRO_PRE_TITLE_TICKS = 72;     // 0x443d(0x48)         @0x4828
constexpr std::size_t INTRO_TITLE_WIPE_TICKS = 18;    // the 0x12 divisor     @0x4851
constexpr std::size_t INTRO_TITLE_FLASH_TICKS = 5;    // 0x4315(.., 5)        @0x4938
constexpr std::size_t INTRO_TITLE_HOLD_TICKS = 9;     // 0x443d(9)            @0x4948
constexpr std::size_t INTRO_TITLE_SETTLE_TICKS = 70;  // 0x4315(.., 0x46)     @0x495a
constexpr std::size_t CREDIT_FADE_TICKS = 50;         // 0x4315(.., 0x32)     @0x49f6
constexpr std::size_t CREDIT_HOLD_TICKS = 50;         // 0x443d(0x32)         @0x49fc
constexpr std::size_t INTRO_OUTRO_FADE_TICKS = 36;    // 0x4b72(pal, 0, 0x24) @0x4a95
// Reaching the end of a road hands over to @0xe58, which keeps drawing and moving for
// 0x48 ticks before the completion banner.
constexpr std::size_t ROAD_END_FLYOFF_TICKS = 72;
// The credit loop runs i = 2..6 (@0x4989/@0x4995) over the seven (CMAP, CMAP, PICT)
// sets loaded after the title, so it shows intro.lzs pictures 4..8. Sets 0 and 1 are
// loaded but never displayed, and picture 9 is never even read.
constexpr std::size_t CREDIT_FIRST_FRAME = 4;
constexpr std::size_t CREDIT_FRAME_COUNT = 5;
constexpr std::size_t CREDIT_FRAME_TICKS = CREDIT_FADE_TICKS * 2 + CREDIT_HOLD_TICKS;

// Linear 0 -> 1 over `span` ticks, matching 0x4315's `100 * ticks / duration`.
float ramp(std::size_t elapsed, std::size_t span) {
    if (span == 0) return 1.0f;
    return std::min(static_cast<float>(elapsed) / static_cast<float>(span), 1.0f);
}

constexpr std::size_t RENDER_ROWS_BEHIND = 3;
constexpr std::size_t RENDER_ROWS_AHEAD = 7;
// Song mapping, from every call to the song loader @0x57a8 (which early-returns if
// the requested song is already playing). Each site is identified by the asset its
// routine loads immediately afterwards:
//   @0x207  song 0 at startup; @0x4586 song 0 then loads intro.lzs   -> INTRO
//   @0x4e41 song 1 then loads mainmenu.lzs                           -> MAIN MENU
//   @0x4cfc song 1 (setmenu.lzs routine)                             -> SETTINGS
//   @0x5174 song 1 then loads gomenu.lzs                             -> LEVEL SELECT
//   @0x2c9  song = rand()%12 + 2, stepped by one if it would repeat the previous
//           (PRNG @0x19c, then div 12 @0x2a4)                        -> GAMEPLAY
// So the intro has its own song, the main menu starts "the main song" (1), that
// keeps playing through the level select, and only starting a road changes it.
//   songs 2..13 = the twelve in-game tracks. Gameplay picks one at RANDOM
//                 (@0x2a4-0x2c8: index = rand % 12 + 2, and if it repeats the
//                 previous choice it steps to (prev + 1) % 12 so the same track
//                 never plays twice running).
constexpr uint8_t INTRO_SONG_INDEX = 0;
constexpr uint8_t MENU_SONG_INDEX = 1;
constexpr uint8_t GAMEPLAY_SONG_FIRST = 2;
constexpr uint8_t GAMEPLAY_SONG_COUNT = 12;

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

// The GOMENU cursor in the original is a single flat index 0..29 (ds:0x933e), laid
// out as two columns of 15: the left column holds worlds 0-4 and the right worlds
// 5-9, each world contributing three roads. Grid maths from the draw loop @0x51ce
// and the cursor placement @0x50e3: column = idx/15, world row = (idx/3)%5,
// road = idx%3.
namespace {
constexpr std::size_t GO_MENU_ENTRIES = 30;
constexpr std::size_t GO_MENU_COLUMN_ENTRIES = 15;
// How long the "Road Completed" banner stays up: the EXE's delay loop is called with
// 0x1b = 27 ticks (@0x2c90 -> 0x443d), i.e. 0.75s at the 36Hz tick.
constexpr std::size_t COMPLETION_MESSAGE_TICKS = 27;

std::size_t go_menu_flat_index(std::size_t world, std::size_t road) {
    return (world / 5) * GO_MENU_COLUMN_ENTRIES + (world % 5) * 3 + road;
}
std::size_t go_menu_world_of(std::size_t flat) {
    return (flat / GO_MENU_COLUMN_ENTRIES) * 5 + (flat / 3) % 5;
}
std::size_t go_menu_road_of(std::size_t flat) { return flat % 3; }
} // namespace

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
        case AppMode::SettingsMenu: tick_settings_menu(input, audio); break;
        case AppMode::GoMenu: tick_go_menu(input, audio); break;
        case AppMode::DemoPlayback: tick_demo(input, audio); break;
        case AppMode::Gameplay: tick_gameplay(input, audio); break;
        case AppMode::Boot:
            mode_ = AppMode::MainMenu;
            break;
    }

    // The song loader itself refuses to do anything while the sound option is set
    // (@0x57bd), so a config with music turned off stays silent everywhere. Sound
    // effects are unaffected -- that check is only in the song loader.
    if (sound_option_ != 0) {
        audio.erase(std::remove_if(audio.begin(), audio.end(),
                                   [](const AudioCommand& command) {
                                       return command.kind ==
                                              AudioCommandKind::PlaySong;
                                   }),
                    audio.end());
    }

    AppTickResult result;
    result.mode = mode_;
    result.render_scene = current_render_scene();
    result.audio_commands = std::move(audio);
    return result;
}

void AttractModeApp::tick_intro(AppInput input, std::vector<AudioCommand>& audio) {
    if (!intro_song_started_) {
        audio.push_back(AudioCommand::play_song(INTRO_SONG_INDEX));
        intro_song_started_ = true;
    }
    // intro.snd fires once the opening fade and its 24-tick pause are done (@0x4768),
    // and only if no key has been seen yet.
    const std::size_t sample_tick = INTRO_BG_FADE_TICKS + INTRO_PRE_SOUND_TICKS;
    if (!intro_sample_started_ && intro_tick_ >= sample_tick) {
        audio.push_back(AudioCommand::play_intro_sample());
        intro_sample_started_ = true;
    }
    // ds:0x54ac is cleared once at @0x4707 and then never again until the intro
    // returns, so the first key press short-circuits every remaining stage and drops
    // straight into the main menu (where the main song starts).
    if (input.skip_requested()) {
        enter_main_menu(audio);
        return;
    }

    // Letting the intro play out returns "no key" (@0x221), and the outer loop then
    // goes to the DEMO, not the menu -- still on song 0. Only a keypress reaches the
    // main menu and its song.
    if (intro_tick_ >= final_credit_end_tick()) {
        start_demo(audio);
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

    // No idle timeout here: the main menu routine @0x4e36 never reads the tick
    // counter and never starts a demo. Attract mode is only ever reached by letting
    // the intro run out, and once a key has taken you to the menu the game stays
    // there. The port used to drop into a demo after five idle seconds.
    (void)navigated;
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

// Settings screen (@0x4c17). Cursor 0..4: positions 0-2 pick the input device
// (ds:0x4526) and 3-4 the sound option (ds:0x4528). Navigation is literally the
// EXE's (@0x4d08-0x4d9c): LEFT and RIGHT walk the five positions in a line, UP goes
// 3 -> 0 and 4 -> 1, and DOWN goes 0 -> 3 and anything already at 3 or more to 4 --
// so DOWN from position 1 or 2 does nothing at all. ENTER applies the highlighted
// position, ESC leaves (and the host then saves skyroads.cfg, as @0x4da5 does).
void AttractModeApp::tick_settings_menu(AppInput input,
                                        std::vector<AudioCommand>& audio) {
    if (input.escape) {
        mode_ = AppMode::MainMenu;
        menu_idle_tick_ = 0;
        return;
    }
    if (input.left && settings_cursor_ != 0) settings_cursor_ -= 1;
    if (input.right && settings_cursor_ < 4) settings_cursor_ += 1;
    if (input.up) {
        if (settings_cursor_ == 3) settings_cursor_ = 0;
        else if (settings_cursor_ == 4) settings_cursor_ = 1;
    }
    if (input.down) {
        if (settings_cursor_ == 0) settings_cursor_ = 3;
        else if (settings_cursor_ >= 3) settings_cursor_ = 4;
    }
    if (input.enter) {
        if (settings_cursor_ <= 2) {
            input_device_ = settings_cursor_;
        } else {
            sound_option_ = settings_cursor_ - 3;
            // @0x4ce4: a non-zero sound option stops the music and parks the loaded
            // song at 0xFFFF, after which the loader early-returns forever. Turning
            // it back on restarts song 1 straight away.
            if (sound_option_ != 0) {
                audio.push_back(AudioCommand::stop_song());
                menu_song_started_ = false;
            } else {
                audio.push_back(AudioCommand::play_song(MENU_SONG_INDEX));
                menu_song_started_ = true;
            }
        }
    }
}

void AttractModeApp::tick_go_menu(AppInput input, std::vector<AudioCommand>& audio) {
    if (input.escape) {
        enter_main_menu(audio);
        return;
    }
    // Exact navigation from the EXE (@0x52cb-0x5305), operating on the flat index:
    //   Up    idx - 1, stopping at 0
    //   Down  idx + 1
    //   Left  idx - 15, or 0 when already in the left column
    //   Right idx + 15
    // followed by a single clamp to the last entry (@0x527f). Note Up/Down walk the
    // flat list continuously, so they cross between the columns (Up from the top of
    // the right column lands on the bottom of the left one) -- and Left from the left
    // column jumps to the very first entry rather than doing nothing.
    const std::size_t entries =
        std::min(GO_MENU_ENTRIES, levels_.size() > 1 ? levels_.size() - 1 : 1);
    std::size_t flat = go_menu_flat_index(selected_world_, selected_level_);
    if (input.up && flat > 0) flat -= 1;
    if (input.down) flat += 1;
    if (input.left) {
        flat = flat >= GO_MENU_COLUMN_ENTRIES ? flat - GO_MENU_COLUMN_ENTRIES : 0;
    }
    if (input.right) flat += GO_MENU_COLUMN_ENTRIES;
    if (flat >= entries) flat = entries - 1;
    selected_world_ = go_menu_world_of(flat);
    selected_level_ = go_menu_road_of(flat);

    if (input.enter) {
        current_level_index_ = selected_road_index();
        start_gameplay(audio, true);
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
    // ESC (ds:0xbaa in the watched-scancode table, tested @0x21e9) is the only key
    // that ends a road, and it returns outcome 7. In the attract loop @0x36a-0x385
    // outcome 7 is the single case that leaves for the main menu -- which is where
    // song 1 finally starts. Every other key just lets the demo run on.
    if (input.escape) {
        enter_main_menu(audio);
        return;
    }
    if (sample_demo_input_for_ship(demo_recording_, demo_session_.ship) ==
        nullptr) {
        restart_intro(audio);
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
        emit_empty_tank_alarm(audio);
        return;
    }

    // Outer flow from the EXE (@0x339-0x3b7): the level routine returns the outcome
    // code, and dying (outcomes 1-5) loops straight back into the SAME road without
    // ever showing the menu. Only completing it (outcome 0) bumps that road's
    // completion count, steps the cursor to the next entry, and returns to the menu.
    if (gameplay_session_.ship.state != ShipState::Alive) {
        // Retry the same road, keeping the track that is already playing.
        start_gameplay(audio, false);
        return;
    }

    if (gameplay_session_.did_win) {
        // Completing a road prints a banner and holds it for 0x1b = 27 ticks, which a
        // keypress can cut short (EXE @0x2c62-0x2c90, delay loop @0x443d). There is no
        // "press a key to continue" -- it simply times out and returns to the menu.
        // Before any of that, the EXE flies the ship on for 0x48 = 72 ticks (@0xe58,
        // entered from @0x23ff once the end of the road is reached and the ship is
        // not dying). That loop forces y_position to 0 and keeps integrating z while
        // redrawing, so the ship dives away below the dashboard and the scenery runs
        // on for two seconds. ESC cuts it short. Only then does the banner appear.
        if (!was_gameover_) {
            was_gameover_ = true;
            win_message_ticks_ = 0;
            road_end_ticks_ = 0;
            gameplay_session_.ship.y_position = 0.0;
            const std::size_t flat =
                go_menu_flat_index(selected_world_, selected_level_);
            // @0x39c is a plain `add WORD PTR [bx], 1` -- a 16-bit counter with no
            // ceiling, so it simply wraps. Nothing reads the magnitude beyond the
            // seven-marker cap in the level select.
            if (flat < road_completions_.size()) {
                road_completions_[flat] = static_cast<uint16_t>(
                    road_completions_[flat] + 1);
            }
        }
        if (road_end_ticks_ < ROAD_END_FLYOFF_TICKS && !input.escape) {
            road_end_ticks_ += 1;
            gameplay_session_.ship.z_position += gameplay_session_.ship.z_velocity;
            return;
        }
        win_message_ticks_ += 1;
        const bool skipped = input.enter || input.space || input.escape;
        if (win_message_ticks_ < COMPLETION_MESSAGE_TICKS && !skipped) return;

        // Then the cursor steps to the next entry and we drop back to the level
        // select (EXE @0x39f), where the new completion marker is visible.
        const std::size_t entries =
            std::min(GO_MENU_ENTRIES, levels_.size() > 1 ? levels_.size() - 1 : 1);
        std::size_t flat = go_menu_flat_index(selected_world_, selected_level_) + 1;
        if (flat >= entries) flat = entries - 1;
        selected_world_ = go_menu_world_of(flat);
        selected_level_ = go_menu_road_of(flat);
        enter_select(audio, true);
        return;
    }

    was_gameover_ = false;
    GameplayFrameResult result =
        gameplay_session_.run_frame(input.gameplay_controls());
    emit_sfx_for_events(result.events, audio);
}

// The empty-tank alarm. The HUD plays SFX 3 on the rising edge of the same 4 Hz blink
// phase that flashes the label (@0x13eb for oxygen, @0x14d4 for fuel), for as long as
// the ship is out of that resource.
void AttractModeApp::emit_empty_tank_alarm(std::vector<AudioCommand>& audio) {
    const ShipState state = gameplay_session_.ship.state;
    const bool empty =
        state == ShipState::OutOfFuel || state == ShipState::OutOfOxygen;
    const bool phase =
        empty && dos_warn_blink_phase(gameplay_session_.frame_index());
    if (phase && !warn_blink_phase_) {
        audio.push_back(AudioCommand::play_sfx(DOS_WARN_SFX));
    }
    warn_blink_phase_ = phase;
}

// Picks the next in-game track. The original chooses at random and only guards
// against repeating the previous one; we step deterministically instead so the
// sequence is reproducible for tests, while still using all twelve and never
// repeating back to back.
uint8_t AttractModeApp::next_gameplay_song() {
    const auto song =
        static_cast<uint8_t>(GAMEPLAY_SONG_FIRST + gameplay_song_pick_);
    gameplay_song_pick_ = (gameplay_song_pick_ + 1) % GAMEPLAY_SONG_COUNT;
    return song;
}

// @0x229: an intro that returns "no key" sets the input source ds:0x9602 to 3, and
// the outer loop then skips the main menu entirely and runs road 0 straight from
// DEMO.REC (@0x26c). Note what is NOT there: any call to the song loader. The whole
// attract mode plays through on song 0.
void AttractModeApp::start_demo(std::vector<AudioCommand>& audio) {
    (void)audio;
    mode_ = AppMode::DemoPlayback;
    menu_idle_tick_ = 0;
    demo_session_ = GameplaySession(levels_[demo_level_index_]);
}

// @0x385: a demo that ends any way other than ESC jumps back to 0x219, which runs
// the intro again -- so attract mode cycles intro, demo, intro, demo, ... The intro
// re-requests song 0 (@0x4586), but the loader early-returns because it is already
// playing (@0x57b1), so the music never restarts.
void AttractModeApp::restart_intro(std::vector<AudioCommand>& audio) {
    mode_ = AppMode::Intro;
    intro_tick_ = 0;
    intro_sample_started_ = false;
    menu_idle_tick_ = 0;
    audio.push_back(AudioCommand::play_song(INTRO_SONG_INDEX));
}

// `switch_song` mirrors where the EXE enters the road loop. Coming from the level
// select it falls through @0x296-0x2cc, which picks a random gameplay track; a death
// instead jumps to 0x339, which is INSIDE the loop and past that pick -- so the track
// carries on across every retry and only changes when you go back to the level select.
void AttractModeApp::start_gameplay(std::vector<AudioCommand>& audio,
                                    bool switch_song) {
    mode_ = AppMode::Gameplay;
    menu_idle_tick_ = 0;
    was_gameover_ = false;
    awaiting_advance_release_ = false;
    win_message_ticks_ = 0;
    // "The End" replaces "Road Completed" when this road is the only one still
    // uncompleted (EXE @0x30e-0x358: count the non-zero completion entries and
    // compare against 29 while this road's own entry is still zero).
    {
        const std::size_t flat = go_menu_flat_index(selected_world_, selected_level_);
        std::size_t completed = 0;
        for (uint16_t c : road_completions_) {
            if (c != 0) completed += 1;
        }
        current_road_is_final_ =
            flat < road_completions_.size() && road_completions_[flat] == 0 &&
            completed == GO_MENU_ENTRIES - 1;
    }
    gameplay_session_ = GameplaySession(levels_[current_level_index_]);
    if (switch_song) {
        audio.push_back(AudioCommand::play_song(next_gameplay_song()));
    }
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
            scene.settings_menu =
                SettingsMenuScene{settings_cursor_, input_device_, sound_option_};
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
    scene.completions = road_completions_;
    scene.gravity = level.gravity;
    scene.fuel = level.fuel;
    scene.oxygen = level.oxygen;
    return scene;
}

std::size_t AttractModeApp::intro_anim_ticks() const {
    return intro_anim_group_count_ * INTRO_ANIM_GROUP_TICKS;
}

IntroSequenceState AttractModeApp::current_intro_scene() const {
    // Stage boundaries, in the order the routine runs them.
    const std::size_t anim_start = INTRO_BG_FADE_TICKS + INTRO_PRE_SOUND_TICKS +
                                   INTRO_POST_SOUND_TICKS;
    const std::size_t anim_end = anim_start + intro_anim_ticks();
    const std::size_t wipe_start = anim_end + INTRO_PRE_TITLE_TICKS;
    const std::size_t flash_start = wipe_start + INTRO_TITLE_WIPE_TICKS;
    const std::size_t hold_start = flash_start + INTRO_TITLE_FLASH_TICKS;
    const std::size_t settle_start = hold_start + INTRO_TITLE_HOLD_TICKS;
    const std::size_t credits_start = settle_start + INTRO_TITLE_SETTLE_TICKS;
    const std::size_t credits_end =
        credits_start + CREDIT_FRAME_TICKS * CREDIT_FRAME_COUNT;

    IntroSequenceState state{};
    state.tick = intro_tick_;

    // The screen fades up from black at the start and back down at the very end.
    state.background_brightness = ramp(intro_tick_, INTRO_BG_FADE_TICKS);
    if (intro_tick_ >= credits_end) {
        state.background_brightness =
            1.0f - ramp(intro_tick_ - credits_end, INTRO_OUTRO_FADE_TICKS);
    }

    // Groups accumulate: whatever has been painted stays painted.
    if (intro_tick_ <= anim_start) {
        state.anim_groups_drawn = 0;
    } else {
        state.anim_groups_drawn =
            std::min((intro_tick_ - anim_start) / INTRO_ANIM_GROUP_TICKS,
                     intro_anim_group_count_);
    }

    state.title_visible = intro_tick_ >= wipe_start;
    state.title_wipe = 1.0f;
    if (intro_tick_ >= wipe_start) {
        state.title_wipe = 1.0f - ramp(intro_tick_ - wipe_start, INTRO_TITLE_WIPE_TICKS);
    }
    if (intro_tick_ < flash_start) {
        state.title_white = 0.0f;
        state.title_mix = 0.0f;
    } else if (intro_tick_ < hold_start) {
        state.title_white = ramp(intro_tick_ - flash_start, INTRO_TITLE_FLASH_TICKS);
        state.title_mix = 0.0f;
    } else if (intro_tick_ < settle_start) {
        state.title_white = 1.0f;
        state.title_mix = 0.0f;
    } else {
        state.title_white =
            1.0f - ramp(intro_tick_ - settle_start, INTRO_TITLE_SETTLE_TICKS);
        state.title_mix = 1.0f;
    }

    if (intro_tick_ >= credits_start && intro_tick_ < credits_end) {
        const std::size_t elapsed = intro_tick_ - credits_start;
        const std::size_t index = elapsed / CREDIT_FRAME_TICKS;
        const std::size_t within = elapsed % CREDIT_FRAME_TICKS;
        state.credit_frame_index = CREDIT_FIRST_FRAME + index;
        if (within < CREDIT_FADE_TICKS) {
            state.credit_mix = ramp(within, CREDIT_FADE_TICKS);
        } else if (within < CREDIT_FADE_TICKS + CREDIT_HOLD_TICKS) {
            state.credit_mix = 1.0f;
        } else {
            state.credit_mix =
                1.0f - ramp(within - CREDIT_FADE_TICKS - CREDIT_HOLD_TICKS,
                            CREDIT_FADE_TICKS);
        }
    }

    return state;
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
    state.is_final_road = !is_demo && current_road_is_final_;
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
    return INTRO_BG_FADE_TICKS + INTRO_PRE_SOUND_TICKS + INTRO_POST_SOUND_TICKS +
           intro_anim_ticks() + INTRO_PRE_TITLE_TICKS + INTRO_TITLE_WIPE_TICKS +
           INTRO_TITLE_FLASH_TICKS + INTRO_TITLE_HOLD_TICKS +
           INTRO_TITLE_SETTLE_TICKS + CREDIT_FRAME_TICKS * CREDIT_FRAME_COUNT +
           INTRO_OUTRO_FADE_TICKS;
}

} // namespace skyroads::core
