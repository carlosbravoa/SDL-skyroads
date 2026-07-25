// Equivalence tests for the reference renderer, mirroring the unit-test blocks in
// the reference renderer suite. These check the same behaviours the reference tests do:
// asset counts, atlas extraction, non-empty frame hashes, and pixel-population
// thresholds in the playfield / ship region / branding band.
#include "check.hpp"
#include "core/core.hpp"
#include "data/data.hpp"
#include "renderer/renderer.hpp"

using namespace skyroads::renderer;
using skyroads::core::AppInput;
using skyroads::core::AttractModeApp;
using skyroads::data::levels_from_roads_archive;
using skyroads::data::load_demo_rec_path;
using skyroads::data::load_roads_lzs_path;

static AttractModeApp make_app() {
    auto roads = load_roads_lzs_path(check::asset("ROADS.LZS"));
    auto demo = load_demo_rec_path(check::asset("DEMO.REC"));
    return AttractModeApp(levels_from_roads_archive(roads), demo);
}

static AttractModeAssets load_assets() {
    return AttractModeAssets::load_from_root(check::assets_dir());
}

static void test_assets_load() {
    AttractModeAssets a = load_assets();
    CHECK_EQ(a.intro.frame_count(), static_cast<std::size_t>(10));
    CHECK_EQ(a.anim.frame_count(), static_cast<std::size_t>(100));
    CHECK_EQ(a.main_menu.frame_count(), static_cast<std::size_t>(3));
    CHECK_EQ(a.help_menu.frame_count(), static_cast<std::size_t>(3));
    CHECK_EQ(a.worlds.size(), static_cast<std::size_t>(10));
    CHECK_EQ(a.trekdat.record_count(), static_cast<std::size_t>(8));
}

static void test_car_atlas() {
    AttractModeAssets a = load_assets();
    auto atlas = CarAtlas::from_archive(a.cars);
    CHECK_TRUE(atlas.has_value());
    // Fixed 24x30-block extraction: 14 explosion sprites (0..13) and 63 flight
    // poses (14..76), each rotated to landscape (30 wide x 24 tall).
    CHECK_EQ(atlas->explosion_frames.size(), static_cast<std::size_t>(14));
    CHECK_EQ(atlas->exact_ship_frames.size(), static_cast<std::size_t>(63));
    CHECK_TRUE(atlas->exact_ship_frames[27].width > atlas->exact_ship_frames[27].height);
}

static void test_intro_menu_non_empty() {
    ReferenceRenderer renderer(load_assets());
    AttractModeApp app = make_app();
    FrameBuffer320x200 intro = renderer.render_scene(app.tick(AppInput{}).render_scene);
    CHECK_TRUE(frame_hash(intro) != 0);

    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    AppInput space;
    space.space = true;
    FrameBuffer320x200 menu = renderer.render_scene(app.tick(space).render_scene);
    CHECK_TRUE(frame_hash(menu) != frame_hash(intro));
}

static void test_gameplay_playfield() {
    ReferenceRenderer renderer(load_assets());
    AttractModeApp app = make_app();
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    AppInput space; space.space = true;
    app.tick(space);
    AppInput enter; enter.enter = true;
    auto gameplay = app.tick(enter);
    FrameBuffer320x200 frame = renderer.render_scene(gameplay.render_scene);

    std::size_t non_black = 0;
    for (std::size_t y = 30; y < 138; ++y) {
        for (std::size_t x = 0; x < frame.width; ++x) {
            const std::size_t o = (y * frame.width + x) * 4;
            if (frame.pixels_rgba[o] || frame.pixels_rgba[o + 1] || frame.pixels_rgba[o + 2]) {
                non_black += 1;
            }
        }
    }
    CHECK_TRUE(non_black > 5000);
}

static void test_gameplay_ship_pixels() {
    ReferenceRenderer renderer(load_assets());
    AttractModeApp app = make_app();
    for (int i = 0; i < 35; ++i) app.tick(AppInput{});
    AppInput space; space.space = true; app.tick(space);
    AppInput enter; enter.enter = true; app.tick(enter);
    AppInput up; up.up_held = true;
    auto gameplay = app.tick(up);
    FrameBuffer320x200 frame = renderer.render_scene(gameplay.render_scene);

    std::size_t ship_pixels = 0;
    for (std::size_t y = 52; y < 104; ++y) {
        for (std::size_t x = 120; x < 200; ++x) {
            const std::size_t o = (y * frame.width + x) * 4;
            const uint8_t r = frame.pixels_rgba[o];
            const uint8_t g = frame.pixels_rgba[o + 1];
            const uint8_t b = frame.pixels_rgba[o + 2];
            if (b > 90 && b > r && b > g) ship_pixels += 1;
        }
    }
    CHECK_TRUE(ship_pixels > 40);
}

static void test_branding() {
    ReferenceRenderer renderer(load_assets());
    FrameBuffer320x200 frame;
    renderer.draw_branding(frame, 184, 2, 1.0f);
    std::size_t bright = 0;
    for (std::size_t y = 184; y < 196; ++y) {
        for (std::size_t x = 40; x < 280; ++x) {
            const std::size_t o = (y * frame.width + x) * 4;
            if (frame.pixels_rgba[o] > 150 && frame.pixels_rgba[o + 1] > 120) bright += 1;
        }
    }
    CHECK_TRUE(bright > 200);
}

CHECK_MAIN_BEGIN()
    test_assets_load();
    test_car_atlas();
    test_intro_menu_non_empty();
    test_gameplay_playfield();
    test_gameplay_ship_pixels();
    test_branding();
CHECK_MAIN_END()
