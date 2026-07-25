// Part of the SkyRoads SDL port
//
// skyroads.cfg: the original's saved settings and per-road progress. The game
// writes 0x42 = 66 bytes from ds:0x4524 (save @0x5770, load @0x571b), laid out as
// 33 little-endian words: word 0 is a checksum, words 1-2 are settings, and words
// 3..32 are the number of times each of the 30 roads has been completed. A missing
// file or a failed checksum resets the whole block to zero (@0x5744-0x5759).
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace skyroads::data {

constexpr std::size_t CONFIG_ROAD_COUNT = 30;
constexpr std::size_t CONFIG_BYTES = 66;

struct GameConfig {
    // The two setting words the original keeps alongside progress. We do not
    // interpret them, but we preserve them so a file stays usable by the game.
    uint16_t setting_a = 0;
    uint16_t setting_b = 0;
    std::array<uint16_t, CONFIG_ROAD_COUNT> road_completions{};
};

// Returns a zeroed config when the file is absent, short, or fails its checksum,
// which is what the original does.
GameConfig load_game_config(const std::string& path);
bool save_game_config(const std::string& path, const GameConfig& config);

// Exposed for tests: the checksum the original computes over the block.
uint16_t game_config_checksum(const GameConfig& config);

} // namespace skyroads::data
