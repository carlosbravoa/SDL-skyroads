// Part of the SkyRoads SDL port
#include "data/config.hpp"

#include <cstdio>
#include <vector>

namespace skyroads::data {
namespace {

constexpr std::size_t CONFIG_WORDS = CONFIG_BYTES / 2;

std::array<uint16_t, CONFIG_WORDS> to_words(const GameConfig& config) {
    std::array<uint16_t, CONFIG_WORDS> words{};
    words[0] = 0; // checksum slot, filled in by the caller
    words[1] = config.setting_a;
    words[2] = config.setting_b;
    for (std::size_t i = 0; i < CONFIG_ROAD_COUNT; ++i) {
        words[3 + i] = config.road_completions[i];
    }
    return words;
}

} // namespace

// Checksum @0x56c3. Note the original advances its pointer only once, before the
// loop, so all 32 iterations read the SAME word (the one after the checksum) and
// accumulate it XOR-ed with the loop counter. That is faithfully reproduced here:
// getting it "right" instead would produce files the original game rejects.
uint16_t game_config_checksum(const GameConfig& config) {
    const std::array<uint16_t, CONFIG_WORDS> words = to_words(config);
    uint16_t sum = 0;
    for (uint16_t i = 1; i < 33; ++i) {
        sum = static_cast<uint16_t>(sum + (words[1] ^ i));
    }
    return sum;
}

GameConfig load_game_config(const std::string& path) {
    GameConfig config;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return config; // absent -> zeroed, as the original does

    std::array<uint8_t, CONFIG_BYTES> raw{};
    const std::size_t read = std::fread(raw.data(), 1, raw.size(), file);
    std::fclose(file);
    if (read != raw.size()) return config;

    auto word_at = [&raw](std::size_t index) -> uint16_t {
        return static_cast<uint16_t>(raw[index * 2] |
                                     (static_cast<uint16_t>(raw[index * 2 + 1]) << 8));
    };
    GameConfig loaded;
    loaded.setting_a = word_at(1);
    loaded.setting_b = word_at(2);
    for (std::size_t i = 0; i < CONFIG_ROAD_COUNT; ++i) {
        loaded.road_completions[i] = word_at(3 + i);
    }
    // A bad checksum resets everything, matching @0x574e-0x5759.
    if (word_at(0) != game_config_checksum(loaded)) return config;
    return loaded;
}

bool save_game_config(const std::string& path, const GameConfig& config) {
    std::array<uint16_t, CONFIG_WORDS> words = to_words(config);
    words[0] = game_config_checksum(config);

    std::array<uint8_t, CONFIG_BYTES> raw{};
    for (std::size_t i = 0; i < CONFIG_WORDS; ++i) {
        raw[i * 2] = static_cast<uint8_t>(words[i] & 0xFF);
        raw[i * 2 + 1] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const std::size_t written = std::fwrite(raw.data(), 1, raw.size(), file);
    std::fclose(file);
    return written == raw.size();
}

} // namespace skyroads::data
