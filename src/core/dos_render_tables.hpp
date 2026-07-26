// Reverse-engineered SKYROADS.EXE runtime render tables, baked into the port.
//
// The DOS build stored these in its runtime data segment (see the RE notes:
// tile-class table at DS:0x0B77, draw-dispatch table at DS:0x0B7F). We interpret
// the executable ONCE (validated by a test that extracts them from the real
// binary — see tests/test_core.cpp `baked_tables_match_exe`) and then reimplement
// the behaviour here as constants, so the shipped port has NO runtime dependency
// on SKYROADS.EXE.
//
// These values are the 30472-byte reference build's render-engine tables.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace skyroads::core {

// DS:0x0B77 — maps a descriptor's low-3 dispatch variant to a tile class.
inline constexpr std::array<uint8_t, 8> DOS_TILE_CLASS_BY_LOW3 = {
    1, 2, 3, 3, 4, 4, 1, 1};

// DS:0x0B7F — draw routine address per dispatch kind. Kinds 6..15 are the
// `ret`/no-op slot (0x3AAD) and never appear in shipped road data.
inline constexpr std::array<uint16_t, 16> DOS_DRAW_DISPATCH_TARGETS = {
    0x2E50, 0x303D, 0x2E9F, 0x2EE1, 0x2F3C, 0x2FB0, 0x3AAD, 0x3AAD,
    0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD};

// DS:0x0322 — shade -> palette index, four bytes per shade. The road is drawn in
// two passes and each uses a DIFFERENT byte of this entry: the forward rasterizer
// (left half, @0x3143) reads byte 0 and the reverse rasterizer (right half, mirrored,
// @0x3180) reads byte 1. Bytes 2 and 3 are the EGA/planar pair.
//
// Byte 0 is the identity, but byte 1 is NOT: shades 0x1F..0x2D remap to 0x2E..0x3C,
// 0x3F to 0x40, and 0x44..0x49 shuffle. Those are exactly the side-wall shades that
// handler 0 produces as `tile colour + 0x1E`, so the right half of every wall, cube
// and tube is drawn from a second, separate band of the road palette. That extra band
// is the shade the port was missing, and the reason tubes looked flat and mis-tinted.
//
// Table stops at 0x49; nothing the road renderer generates goes above that.
inline constexpr std::size_t DOS_SHADE_LUT_SIZE = 0x4A;
inline constexpr std::array<uint8_t, DOS_SHADE_LUT_SIZE> DOS_SHADE_LUT_FORWARD = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x47, 0x46, 0x45, 0x44,
    0x45, 0x46};
inline constexpr std::array<uint8_t, DOS_SHADE_LUT_SIZE> DOS_SHADE_LUT_REVERSE = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x2E, 0x2F, 0x30, 0x31, 0x32,
    0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x3D, 0x3E, 0x40, 0x00, 0x41, 0x42, 0x43, 0x46, 0x45, 0x44, 0x45,
    0x46, 0x47};

// DS:0x065E — the ship's ground shadow, five pre-drawn silhouettes of 29x9 pixels
// picked by how far the ship is above the surface below it. The linear renderer
// @0x33e1 computes `level = hover / 5` and draws nothing at all once that reaches 5,
// so the shadow disappears above 25 units of altitude.
//
// It is not a translucent blob: the blit at @0x3437-0x344a READS the screen pixel and
// remaps its palette index -- 0x3D becomes 0x40, index 0 (sky) is left alone, indices
// below 0x10 (the road's top-surface shades) gain 0x2D, and anything else is
// untouched. So the shadow simply moves the road surface into the same darker
// 0x2E..0x3C band that the mirrored half of the road already uses, and can never fall
// on the sky or on a wall.
//
// One bit per pixel, bit 0 = leftmost column.
inline constexpr std::size_t DOS_SHADOW_LEVELS = 5;
inline constexpr std::size_t DOS_SHADOW_ROWS = 9;
inline constexpr std::size_t DOS_SHADOW_COLUMNS = 29;
// hover / DOS_SHADOW_STEP selects the level; >= DOS_SHADOW_LEVELS means no shadow.
inline constexpr int32_t DOS_SHADOW_STEP = 5;
// Screen row of the shadow's top = (157 - ship_y) + DOS_SHADOW_Y_OFFSET + hover,
// which pins it to the surface the ship is flying over (@0x33fe-0x3408).
inline constexpr int32_t DOS_SHADOW_Y_OFFSET = 16;
inline constexpr std::array<std::array<uint32_t, DOS_SHADOW_ROWS>, DOS_SHADOW_LEVELS>
    DOS_SHIP_SHADOW_MASKS = {{
        {0x0007FE00u, 0x001FFF80u, 0x003FFFC0u, 0x003FFFC0u, 0x001FFF80u, 0x00FFFFF0u,
         0x03FFFFFCu, 0x07FBFEFEu, 0x03F1FC7Cu},
        {0x00000000u, 0x0001F800u, 0x001FFF80u, 0x003FFFC0u, 0x001FFF80u, 0x00FFFFF0u,
         0x00FFFFF0u, 0x007DFDE0u, 0x00000000u},
        {0x00000000u, 0x00000000u, 0x0003FC00u, 0x000FFF00u, 0x0007FE00u, 0x003FFFC0u,
         0x001EFB80u, 0x00000000u, 0x00000000u},
        {0x00000000u, 0x00000000u, 0x00000000u, 0x0003FC00u, 0x0007FE00u, 0x000F7700u,
         0x00000000u, 0x00000000u, 0x00000000u},
        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001F800u, 0x0003AC00u,
         0x00000000u, 0x00000000u, 0x00000000u},
    }};

// The shadow's palette remap, straight from @0x343a-0x3448.
inline uint8_t dos_shadow_shade(uint8_t shade) {
    if (shade == 0x3D) return 0x40;
    if (shade == 0x00) return 0x00; // sky: never shadowed
    if (shade < 0x10) return static_cast<uint8_t>(shade + 0x2D);
    return shade;
}

// DS:0x013C — the dashboard's 4x5 digit glyphs, 20 bytes each, drawn by 0xfc6 via
// the expander @0xeb5: byte 0 becomes palette index 0 (black, the digit strokes) and
// bytes 1 and 2 become 0x61 and 0x62, the two tan shades of the readout window.
// dashbrd.lzs is loaded at palette base 0x5C, so those are its CMAP entries 5 and 6.
inline constexpr std::size_t DOS_DIGIT_WIDTH = 4;
inline constexpr std::size_t DOS_DIGIT_HEIGHT = 5;
// Successive digits sit five pixels apart (@0x10c1) with the units digit rightmost.
inline constexpr int32_t DOS_DIGIT_ADVANCE = 5;
inline constexpr std::array<std::array<uint8_t, 20>, 10> DOS_DIGIT_GLYPHS = {{
    {0, 0, 0, 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 0, 0, 0},
    {1, 1, 1, 0, 1, 2, 2, 0, 1, 1, 1, 0, 1, 2, 2, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 0, 0, 2, 2, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 2, 2, 0, 0, 0, 0, 0, 1, 2, 2, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 2, 2, 1, 0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 2, 2, 1, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 2, 2, 0, 1, 1, 1, 0, 1, 2, 2, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 1, 2, 2, 0, 1, 1, 1, 0},
}};

// The GRAV-O-METER readout: `(gravity - 3) * 100`, four digits, at (96, 156)
// (@0x2ba7-0x2bba). Gravity 8 -- every shipped road -- therefore shows "500".
inline constexpr int32_t DOS_GRAVITY_READOUT_X = 96;
inline constexpr int32_t DOS_GRAVITY_READOUT_Y = 156;
inline constexpr std::size_t DOS_GRAVITY_READOUT_DIGITS = 4;
inline int32_t dos_gravity_readout(int32_t gravity) { return (gravity - 3) * 100; }

// Empty-tank indicators. When a tank runs dry the HUD swaps two palette entries over
// a small rectangle on the dashboard, which makes the label flash, and plays SFX 3:
//   oxygen empty (state 5, @0x1397-0x13f0): 7x7 at (160, 161)  -- the "O2" label
//   fuel empty   (state 4, @0x1480-0x14d9): 16x5 at (155, 169) -- the "FUEL" label
// The swap (@0x11d5) exchanges palette 0x63 and 0x64, i.e. dashbrd.lzs CMAP entries 7
// and 8, so the label alternates between its normal near-white and red.
inline constexpr int32_t DOS_OXYGEN_WARN_X = 160;
inline constexpr int32_t DOS_OXYGEN_WARN_Y = 161;
inline constexpr int32_t DOS_OXYGEN_WARN_W = 7;
inline constexpr int32_t DOS_OXYGEN_WARN_H = 7;
inline constexpr int32_t DOS_FUEL_WARN_X = 155;
inline constexpr int32_t DOS_FUEL_WARN_Y = 169;
inline constexpr int32_t DOS_FUEL_WARN_W = 16;
inline constexpr int32_t DOS_FUEL_WARN_H = 5;
inline constexpr std::size_t DOS_WARN_PALETTE_A = 7; // 0x63
inline constexpr std::size_t DOS_WARN_PALETTE_B = 8; // 0x64
inline constexpr uint8_t DOS_WARN_SFX = 3;
// The alarm is a square wave on the game tick: @0x124b computes
// `phase = (tick % 9) > 4`, giving four ticks on and five off = about 4 Hz. The HUD
// only acts when the phase CHANGES (it keeps the previous value in ds:0xaf2e, zeroed
// at road start @0x2b3a), swapping the two colours on every edge -- so the label is
// swapped throughout the high phase -- and plays the beep on the rising edge only.
inline constexpr std::size_t DOS_WARN_BLINK_PERIOD = 9;
inline constexpr std::size_t DOS_WARN_BLINK_THRESHOLD = 4;
inline bool dos_warn_blink_phase(std::size_t tick) {
    return (tick % DOS_WARN_BLINK_PERIOD) > DOS_WARN_BLINK_THRESHOLD;
}

// Replaces skyroads::data::ExeDispatchEntry in the planner so nothing in the
// runtime path names the executable.
struct DispatchEntry {
    std::size_t index;
    uint16_t target;
    std::optional<std::string> label; // empty == unknown target
};

uint8_t dos_tile_class(uint8_t dispatch_variant_low3);
DispatchEntry dos_dispatch_entry(std::size_t dispatch_kind);

} // namespace skyroads::core
