// Part of the SkyRoads SDL port
//
// The native platform host. It calls the SDL2 C API directly (SDL2 is already a
// C library, so no binding shim is needed). The game loop, fixed 70 Hz stepping,
// key edge/hold latching, and queued-audio watermarking live here.
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio.hpp"
#include "core/core.hpp"
#include "data/config.hpp"
#include "data/data.hpp"
#include "renderer/renderer.hpp"

using namespace skyroads;

namespace {

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 960;
// The DOS game reprograms the PIT to 180.02 Hz (divisor 0x19E4) and ticks the
// game/physics clock once every 5 interrupts -> ~36 Hz. Matching that rate is
// essential: at the old 70 Hz the whole game ran ~1.94x too fast (and jumps felt
// low/snappy instead of the original's floatier ascent). See re/NOTES.md.
// Default physics rate. The exact DOS design rate is ~36 Hz (see re/NOTES.md),
// but SkyRoads runs faster than that on quick hardware / DOSBox, so this is
// tunable live with the +/- keys to match a reference. Range clamped below.
constexpr uint64_t SIMULATION_HZ_DEFAULT = 36;
constexpr uint64_t SIMULATION_HZ_MIN = 12;
constexpr uint64_t SIMULATION_HZ_MAX = 120;
constexpr int MAX_CATCH_UP_STEPS = 4;
constexpr uint16_t AUDIO_DEVICE_BUFFER_SAMPLES = 1024;
constexpr std::size_t AUDIO_QUEUE_LOW_WATER_SAMPLES = 2048;
constexpr std::size_t AUDIO_QUEUE_TARGET_SAMPLES = 4096;

struct KeyEdges {
    bool up, down, left, right, debug_toggle, enter, escape, space, quit;
    bool rate_up, rate_down;
};

struct HostInput {
    core::AppInput app;
    bool debug_toggle = false;
    bool quit = false;
    bool rate_up = false;
    bool rate_down = false;
};

bool take_edge(bool& previous, bool current) {
    const bool edge = current && !previous;
    previous = current;
    return edge;
}

struct KeyLatch {
    bool up = false, down = false, left = false, right = false;
    bool debug_toggle = false, enter = false, escape = false, space = false, quit = false;
    bool rate_up = false, rate_down = false;

    HostInput sample(const Uint8* kb) {
        const KeyEdges current{
            static_cast<bool>(kb[SDL_SCANCODE_UP] || kb[SDL_SCANCODE_W]),
            static_cast<bool>(kb[SDL_SCANCODE_DOWN] || kb[SDL_SCANCODE_S]),
            static_cast<bool>(kb[SDL_SCANCODE_LEFT] || kb[SDL_SCANCODE_A]),
            static_cast<bool>(kb[SDL_SCANCODE_RIGHT] || kb[SDL_SCANCODE_D]),
            static_cast<bool>(kb[SDL_SCANCODE_TAB]),
            static_cast<bool>(kb[SDL_SCANCODE_RETURN]),
            static_cast<bool>(kb[SDL_SCANCODE_ESCAPE]),
            static_cast<bool>(kb[SDL_SCANCODE_SPACE]),
            static_cast<bool>(kb[SDL_SCANCODE_Q]),
            static_cast<bool>(kb[SDL_SCANCODE_EQUALS] || kb[SDL_SCANCODE_KP_PLUS]),
            static_cast<bool>(kb[SDL_SCANCODE_MINUS] || kb[SDL_SCANCODE_KP_MINUS])};

        HostInput out;
        out.debug_toggle = take_edge(debug_toggle, current.debug_toggle);
        out.quit = take_edge(quit, current.quit);
        out.rate_up = take_edge(rate_up, current.rate_up);
        out.rate_down = take_edge(rate_down, current.rate_down);
        out.app.up = take_edge(up, current.up);
        out.app.down = take_edge(down, current.down);
        out.app.left = take_edge(left, current.left);
        out.app.right = take_edge(right, current.right);
        out.app.enter = take_edge(enter, current.enter);
        out.app.escape = take_edge(escape, current.escape);
        out.app.space = take_edge(space, current.space);
        out.app.up_held = current.up;
        out.app.down_held = current.down;
        out.app.left_held = current.left;
        out.app.right_held = current.right;
        out.app.enter_held = current.enter;
        out.app.space_held = current.space;
        return out;
    }
};

core::AppInput held_only_input(const core::AppInput& in) {
    core::AppInput out;
    out.up_held = in.up_held;
    out.down_held = in.down_held;
    out.left_held = in.left_held;
    out.right_held = in.right_held;
    out.enter_held = in.enter_held;
    out.space_held = in.space_held;
    return out;
}

const char* mode_label(core::AppMode mode) {
    switch (mode) {
        case core::AppMode::Intro: return "Intro";
        case core::AppMode::MainMenu: return "Main Menu";
        case core::AppMode::HelpMenu: return "Help";
        case core::AppMode::SettingsMenu: return "Settings";
        case core::AppMode::DemoPlayback: return "Demo";
        case core::AppMode::Boot: return "Boot";
        case core::AppMode::GoMenu: return "Go";
        case core::AppMode::Gameplay: return "Gameplay";
    }
    return "?";
}

std::string window_title(core::AppMode mode, renderer::DebugViewMode debug_view,
                         uint64_t sim_hz) {
    std::string t = std::string("SkyRoads Native | ") + mode_label(mode);
    t += " | " + std::to_string(sim_hz) + " Hz";
    if (debug_view != renderer::DebugViewMode::Off) {
        t += " | Debug ";
        t += renderer::debug_label(debug_view);
    }
    return t;
}

bool commands_require_flush(const std::vector<core::AudioCommand>& commands) {
    for (const auto& c : commands) {
        switch (c.kind) {
            case core::AudioCommandKind::PlaySong:
            case core::AudioCommandKind::StopSong:
            case core::AudioCommandKind::PlayIntroSample:
            case core::AudioCommandKind::StopAllSamples:
                return true;
            default:
                break;
        }
    }
    return false;
}

std::size_t queued_samples(SDL_AudioDeviceID dev) {
    return SDL_GetQueuedAudioSize(dev) / sizeof(int16_t);
}

void fill_audio_queue(SDL_AudioDeviceID dev, audio::AudioMixer& mixer) {
    const std::size_t queued = queued_samples(dev);
    if (queued >= AUDIO_QUEUE_LOW_WATER_SAMPLES) return;
    const std::size_t needed =
        AUDIO_QUEUE_TARGET_SAMPLES > queued ? AUDIO_QUEUE_TARGET_SAMPLES - queued : 0;
    if (needed == 0) return;
    std::vector<int16_t> samples = mixer.render_i16(needed);
    SDL_QueueAudio(dev, samples.data(),
                   static_cast<Uint32>(samples.size() * sizeof(int16_t)));
}

void apply_audio_commands(audio::AudioMixer& mixer, SDL_AudioDeviceID dev,
                          const std::vector<core::AudioCommand>& commands) {
    if (commands.empty()) return;
    mixer.apply_commands(commands);
    if (commands_require_flush(commands)) SDL_ClearQueuedAudio(dev);
    fill_audio_queue(dev, mixer);
}

void print_controls(const std::string& source_root) {
    std::printf("SkyRoads native attract-mode demo\n");
    std::printf("assets: %s\n", source_root.c_str());
    std::printf("controls:\n");
    std::printf("  Up / Down  menu navigation, throttle, brake\n");
    std::printf("  Left / Right  steer\n");
    std::printf("  Enter      select, restart after crash/win\n");
    std::printf("  Space      skip intro, jump, restart after crash/win\n");
    std::printf("  Tab        cycle debug views\n");
    std::printf("  Escape     back to menu\n");
    std::printf("  + / -      physics rate up/down (shown in the title bar)\n");
    std::printf("  Q          quit\n");
}

int run(const std::string& source_root) {
    auto roads = data::load_roads_lzs_path(source_root + "/ROADS.LZS");
    auto demo = data::load_demo_rec_path(source_root + "/DEMO.REC");
    auto levels = data::levels_from_roads_archive(roads);
    if (levels.empty()) {
        std::fprintf(stderr, "error: ROADS.LZS did not contain any playable levels\n");
        return 1;
    }

    renderer::ReferenceRenderer reference_renderer(
        renderer::AttractModeAssets::load_from_root(source_root));
    audio::AudioMixer audio_mixer(audio::AttractAudioAssets::load_from_root(source_root));
    core::AttractModeApp app(std::move(levels), demo);

    // Progress lives in skyroads.cfg next to the game data, as in the original.
    const std::string config_path = source_root + "/skyroads.cfg";
    data::GameConfig config = data::load_game_config(config_path);
    {
        std::array<uint8_t, 30> counts{};
        for (std::size_t i = 0; i < counts.size(); ++i) {
            counts[i] = static_cast<uint8_t>(std::min<uint16_t>(
                config.road_completions[i], 255));
        }
        app.set_road_completions(counts);
    }
    auto persist_progress = [&]() {
        const auto& counts = app.road_completions();
        bool changed = false;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (config.road_completions[i] != counts[i]) {
                config.road_completions[i] = counts[i];
                changed = true;
            }
        }
        if (changed) data::save_game_config(config_path, config);
    };

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("SkyRoads Native", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    SDL_Renderer* presenter =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* texture =
        SDL_CreateTexture(presenter, SDL_PIXELFORMAT_RGBA32,
                          SDL_TEXTUREACCESS_STREAMING, 320, 200);

    SDL_AudioSpec want{};
    want.freq = static_cast<int>(audio_mixer.output_sample_rate());
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = AUDIO_DEVICE_BUFFER_SAMPLES;
    want.callback = nullptr; // queue API
    SDL_AudioSpec have{};
    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);

    print_controls(source_root);

    core::AppTickResult initial = app.tick(core::AppInput{});
    apply_audio_commands(audio_mixer, audio_device, initial.audio_commands);
    fill_audio_queue(audio_device, audio_mixer);
    SDL_PauseAudioDevice(audio_device, 0);

    core::AppMode current_mode = initial.mode;
    core::RenderScene current_scene = initial.render_scene;
    renderer::DebugViewMode debug_view = renderer::DebugViewMode::Off;
    uint64_t sim_hz = SIMULATION_HZ_DEFAULT;
    SDL_SetWindowTitle(window, window_title(current_mode, debug_view, sim_hz).c_str());

    using clock = std::chrono::steady_clock;
    auto timestep = std::chrono::nanoseconds(1'000'000'000 / sim_hz);
    auto next_tick = clock::now() + timestep;
    KeyLatch latch;
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }
        SDL_PumpEvents();
        const HostInput input = latch.sample(SDL_GetKeyboardState(nullptr));
        if (input.quit) break;
        if (input.debug_toggle) {
            debug_view = renderer::debug_next(debug_view);
            SDL_SetWindowTitle(window, window_title(current_mode, debug_view, sim_hz).c_str());
        }
        // Live physics-rate tuning with +/- (until we lock the faithful rate).
        if (input.rate_up || input.rate_down) {
            const uint64_t step = 2;
            if (input.rate_up && sim_hz + step <= SIMULATION_HZ_MAX) sim_hz += step;
            if (input.rate_down && sim_hz >= SIMULATION_HZ_MIN + step) sim_hz -= step;
            timestep = std::chrono::nanoseconds(1'000'000'000 / sim_hz);
            next_tick = clock::now() + timestep;
            std::printf("physics rate: %llu Hz\n", static_cast<unsigned long long>(sim_hz));
            SDL_SetWindowTitle(window, window_title(current_mode, debug_view, sim_hz).c_str());
        }

        int step_count = 0;
        bool consumed_input = false;
        auto now = clock::now();
        while (now >= next_tick && step_count < MAX_CATCH_UP_STEPS) {
            core::AppInput app_input;
            if (consumed_input) {
                app_input = held_only_input(input.app);
            } else {
                consumed_input = true;
                app_input = input.app;
            }
            core::AppTickResult tick = app.tick(app_input);
            // The original saves as soon as a road is completed; this only writes
            // when a count actually changed, so it is cheap to check each tick.
            persist_progress();
            apply_audio_commands(audio_mixer, audio_device, tick.audio_commands);
            if (tick.mode != current_mode) {
                current_mode = tick.mode;
                SDL_SetWindowTitle(window, window_title(current_mode, debug_view, sim_hz).c_str());
            }
            current_scene = tick.render_scene;
            next_tick += timestep;
            step_count += 1;
        }
        if (now > next_tick + timestep) next_tick = now + timestep;

        fill_audio_queue(audio_device, audio_mixer);

        renderer::FrameBuffer320x200 frame =
            reference_renderer.render_scene_with_debug(current_scene, debug_view);
        SDL_UpdateTexture(texture, nullptr, frame.pixels_rgba.data(),
                          static_cast<int>(frame.width) * 4);
        SDL_SetRenderDrawColor(presenter, 0, 0, 0, 255);
        SDL_RenderClear(presenter);
        SDL_RenderCopy(presenter, texture, nullptr, nullptr);
        SDL_RenderPresent(presenter);

        const auto sleep_for =
            std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         next_tick - clock::now()),
                     std::chrono::nanoseconds(2'000'000));
        if (sleep_for.count() > 0) std::this_thread::sleep_for(sleep_for);
    }

    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(presenter);
    SDL_DestroyWindow(window);
    persist_progress();
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string source_root = ".";
    if (argc > 1) {
        const std::string first = argv[1];
        if (first == "-h" || first == "--help" || argc > 2) {
            std::fprintf(stderr, "usage: skyroads-sdl [source_root]\n");
            return first == "-h" || first == "--help" ? 0 : 1;
        }
        source_root = first;
    }
    try {
        return run(source_root);
    } catch (const data::Error& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
