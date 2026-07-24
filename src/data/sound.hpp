// Part of the SkyRoads SDL port
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

constexpr uint32_t SAMPLE_RATE_PCM_8K = 8000;

struct Pcm8Sample {
    uint32_t sample_rate = 0;
    Bytes samples;
    std::size_t sample_count() const { return samples.size(); }
    double duration_seconds() const {
        return static_cast<double>(samples.size()) /
               static_cast<double>(sample_rate);
    }
};

struct SfxEntry {
    std::size_t index = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    Pcm8Sample sample;
};

struct SfxBank {
    std::size_t source_len = 0;
    uint32_t sample_rate = 0;
    std::vector<SfxEntry> effects;
    std::size_t effect_count() const { return effects.size(); }
};

Pcm8Sample load_intro_snd_bytes(const Bytes& data);
Pcm8Sample load_intro_snd_path(const std::string& path);
SfxBank load_sfx_snd_bytes(const Bytes& data);
SfxBank load_sfx_snd_path(const std::string& path);

} // namespace skyroads::data
