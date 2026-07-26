// Part of the SkyRoads SDL port
//
// The CPU reference renderer: composes intro/menu/help/settings art, world
// backdrops, the DOS TREKDAT road pass, ship sprites, dashboard, and gauges
// into a 320x200 RGBA framebuffer. Mirrors the reference module's types and
// functions 1:1 so `frame_hash` output can be diffed against the reference reference.
//
// The road is drawn by the decoded DOS TREKDAT span pipeline (scene_draw @0x2d03):
// pre-baked run-length spans out of the eight scroll-phase snapshots, dispatched per
// road cell, with the right half produced by the reverse rasterizer.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/app.hpp"
#include "data/dashboard.hpp"
#include "data/image.hpp"
#include "data/level.hpp"
#include "data/trekdat.hpp"

namespace skyroads::renderer {

using skyroads::core::DemoPlaybackState;
using skyroads::core::GoMenuScene;
using skyroads::core::HelpMenuScene;
using skyroads::core::IntroSequenceState;
using skyroads::core::MainMenuScene;
using skyroads::core::RenderScene;
using skyroads::core::RoadRenderRow;
using skyroads::core::SettingsMenuScene;
using skyroads::data::Bytes;
using skyroads::data::HudFragmentPack;
using skyroads::data::ImageArchive;
using skyroads::data::ImageFrame;
using skyroads::data::LevelCell;
using skyroads::data::RgbColor;
using skyroads::data::TrekdatArchive;

struct FrameBuffer320x200 {
    uint16_t width;
    uint16_t height;
    Bytes pixels_rgba;
    // The DOS build renders into an 8-bit paletted buffer, and the ship's shadow
    // works by READING that buffer back and remapping the index it finds. This plane
    // keeps the road shade behind each pixel so the shadow can do the same; the road
    // pass fills it, the ship marks its own pixels SHADE_NOT_ROAD, everything else
    // leaves 0 (which the shadow treats as sky and skips).
    Bytes shade_plane;
    static constexpr uint8_t SHADE_NOT_ROAD = 0xFF;

    FrameBuffer320x200();
    void clear(RgbColor color);
    void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                   RgbColor color);
    void set_pixel(std::size_t x, std::size_t y, RgbColor color);
    void set_pixel(std::size_t x, std::size_t y, RgbColor color, uint8_t shade);
    void blend_pixel(std::size_t x, std::size_t y, RgbColor color, float alpha);
    uint8_t shade_at(std::size_t x, std::size_t y) const;
};

struct AttractModeAssets {
    ImageArchive intro;
    ImageArchive anim;
    ImageArchive main_menu;
    ImageArchive help_menu;
    ImageArchive settings_menu;
    ImageArchive go_menu;
    ImageArchive cars;
    std::vector<ImageArchive> worlds;
    ImageArchive dashboard;
    TrekdatArchive trekdat;
    HudFragmentPack oxygen_gauge;
    HudFragmentPack fuel_gauge;
    HudFragmentPack speed_gauge;

    static AttractModeAssets load_from_root(const std::string& source_root);
};

enum class DebugViewMode {
    Off,
    Overlay,
    Geometry,
    TopDown,
};

DebugViewMode debug_next(DebugViewMode mode);
const char* debug_label(DebugViewMode mode);

// ---- ship visual derivation (public so the placement helpers can share it) --

enum class ShipSpriteKind { Alive, Exploding, Destroyed };
enum class ShipBank { Left, Center, Right };

struct DerivedShipVisualState {
    ShipSpriteKind sprite_kind;
    ShipBank bank;
    bool thrust_on;
    bool jumping;
    std::size_t explosion_frame;
    std::optional<std::size_t> exact_ship_frame_index;
    int32_t ship_screen_bias_x;
    int32_t vertical_offset_y;
    // Height of the surface currently supporting the ship (the road, or the top of
    // a raised block), so the shadow can sit on it instead of a fixed row.
    double support_y;
    bool on_surface;
    // False once the ship is dead: a crashed ship has no shadow, and a ship that
    // fell off the road has no road left under it to cast one on.
    bool casts_shadow;
    // Altitude above that surface in whole world units -- ds:0xe40, the value
    // scene_draw uses to pick which of the five shadow silhouettes to draw.
    int32_t hover_units;
};

struct ShipScreenPlacement {
    int32_t sprite_center_x;
    int32_t sprite_center_y;
    int32_t shadow_center_x;
    int32_t shadow_center_y;
    // The exact DOS blit corner: left column = x - 110, top row = 157 - y
    // (@0x324e-0x325a). The shadow is placed relative to these, not to the centre.
    int32_t sprite_left_x;
    int32_t sprite_top_y;
};

struct CarAtlas {
    std::vector<ImageFrame> explosion_frames;
    std::vector<ImageFrame> exact_ship_frames;
    std::vector<ImageFrame> alive_left;
    std::vector<ImageFrame> alive_center;
    std::vector<ImageFrame> alive_right;
    std::vector<ImageFrame> jump_left;
    std::vector<ImageFrame> jump_center;
    std::vector<ImageFrame> jump_right;
    ImageFrame destroyed;

    static std::optional<CarAtlas> from_archive(const ImageArchive& archive);
    const ImageFrame& select_sprite(const DerivedShipVisualState& visual,
                                    std::size_t frame_index) const;
};

class ReferenceRenderer {
public:
    explicit ReferenceRenderer(AttractModeAssets assets);

    const AttractModeAssets& assets() const { return assets_; }
    FrameBuffer320x200 render_scene(const RenderScene& scene) const;
    FrameBuffer320x200 render_scene_with_debug(const RenderScene& scene,
                                               DebugViewMode debug_view) const;

private:
    void render_intro(FrameBuffer320x200& frame,
                      const IntroSequenceState& scene) const;
    void render_main_menu(FrameBuffer320x200& frame,
                          const MainMenuScene& scene) const;
    void render_help_menu(FrameBuffer320x200& frame,
                          const HelpMenuScene& scene) const;
    void render_settings_menu(FrameBuffer320x200& frame,
                              const SettingsMenuScene& scene) const;
    void render_go_menu(FrameBuffer320x200& frame,
                        const GoMenuScene& scene) const;
    void render_play_scene(FrameBuffer320x200& frame,
                           const DemoPlaybackState& scene) const;
    void render_play_scene_with_debug(FrameBuffer320x200& frame,
                                      const DemoPlaybackState& scene,
                                      DebugViewMode debug_view) const;
    bool draw_demo_rows_before_ship(FrameBuffer320x200& frame,
                                    const DemoPlaybackState& scene) const;
    void draw_demo_rows_after_ship(FrameBuffer320x200& frame,
                                   const DemoPlaybackState& scene) const;
    void draw_demo_rows_fallback(FrameBuffer320x200& frame,
                                 const DemoPlaybackState& scene) const;
    void draw_ship_sprite(FrameBuffer320x200& frame, std::size_t frame_index,
                          const DerivedShipVisualState& visual,
                          ShipScreenPlacement placement) const;
    void draw_gauge(FrameBuffer320x200& frame, const HudFragmentPack& pack,
                    std::size_t level) const;
    void draw_dashboard_number(FrameBuffer320x200& frame, int32_t x, int32_t y,
                               int32_t value, std::size_t digits) const;
    void draw_rom_text(FrameBuffer320x200& frame, int32_t x, int32_t y,
                       const std::string& text, RgbColor color) const;
    RgbColor dashboard_palette_color(std::size_t index) const;
    void draw_empty_tank_warning(FrameBuffer320x200& frame,
                                 const DemoPlaybackState& scene) const;
    void draw_archive_frame(FrameBuffer320x200& frame,
                            const ImageArchive& archive, std::size_t frame_index,
                            float alpha, float brightness) const;
    void draw_archive_frame_reveal(FrameBuffer320x200& frame,
                                   const ImageArchive& archive,
                                   std::size_t frame_index, float progress,
                                   float brightness) const;
    // Draws a picture under the intro's palette animation: `mix` slides from the
    // frame's first CMAP to its second, `white` flashes the result out to full
    // intensity, and `wipe` is the fraction of each row still left as background by
    // the interlaced title wipe (even rows uncover from the right, odd from the
    // left, exactly as the two 0x4184 calls @0x489b/@0x48b1 do).
    void draw_intro_picture(FrameBuffer320x200& frame, const ImageFrame& fragment,
                            float mix, float white, float brightness,
                            float wipe) const;
    void draw_fragment(FrameBuffer320x200& frame, const ImageFrame& fragment,
                       float alpha, float brightness,
                       float horizontal_fraction) const;
    void draw_sprite(FrameBuffer320x200& frame, const ImageFrame& sprite,
                     int32_t dest_x, int32_t dest_y, std::size_t scale) const;
    void draw_projected_slice(FrameBuffer320x200& frame,
                              const struct ProjectedRoadSlice& slice) const;
    void draw_ship_shadow(FrameBuffer320x200& frame,
                          const DerivedShipVisualState& visual,
                          ShipScreenPlacement placement) const;
    void draw_text_centered(FrameBuffer320x200& frame, const std::string& text,
                            int32_t y, RgbColor color, std::size_t scale) const;
    void draw_text(FrameBuffer320x200& frame, int32_t x, int32_t y,
                   const std::string& text, RgbColor color,
                   std::size_t scale) const;
    void draw_debug_overlay(FrameBuffer320x200& frame,
                            const DemoPlaybackState& scene) const;
    void render_play_geometry_debug(FrameBuffer320x200& frame,
                                    const DemoPlaybackState& scene) const;
    void render_play_topdown_debug(FrameBuffer320x200& frame,
                                   const DemoPlaybackState& scene) const;
    void draw_debug_hud_panel(FrameBuffer320x200& frame,
                              const DemoPlaybackState& scene,
                              DebugViewMode mode) const;
    void draw_projected_slice_guides(
        FrameBuffer320x200& frame,
        const std::vector<struct ProjectedRoadSlice>& slices) const;
    void draw_ship_debug_guides(FrameBuffer320x200& frame,
                                const DemoPlaybackState& scene,
                                const DerivedShipVisualState& visual,
                                ShipScreenPlacement placement) const;
    void draw_topdown_inset(FrameBuffer320x200& frame,
                            const DemoPlaybackState& scene) const;
    void draw_topdown_map(FrameBuffer320x200& frame,
                          const DemoPlaybackState& scene, int32_t x, int32_t y,
                          int32_t w, int32_t h, bool large) const;

    AttractModeAssets assets_;
    std::optional<CarAtlas> car_atlas_;
};

uint64_t frame_hash(const FrameBuffer320x200& frame);

DerivedShipVisualState derive_ship_visual_state(const DemoPlaybackState& scene);

} // namespace skyroads::renderer
