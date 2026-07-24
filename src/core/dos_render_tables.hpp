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
