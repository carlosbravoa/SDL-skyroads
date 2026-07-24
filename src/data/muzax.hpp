// Part of the SkyRoads SDL port
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

struct MuzaxSongHeader {
    std::size_t index = 0;
    uint16_t start_pos = 0;
    uint16_t num_instruments = 0;
    uint16_t uncompressed_length = 0;
    bool is_empty() const {
        return start_pos == 0 && num_instruments == 0 &&
               uncompressed_length == 0;
    }
};

struct MuzaxOscillator {
    bool tremolo;
    bool vibrato;
    bool sound_sustaining;
    bool key_scaling;
    uint8_t multiplication;
    uint8_t key_scale_level;
    uint8_t output_level;
    uint8_t attack_rate;
    uint8_t decay_rate;
    uint8_t sustain_level;
    uint8_t release_rate;
    uint8_t wave_form;
};

struct MuzaxInstrument {
    std::size_t index;
    std::array<uint8_t, 16> raw;
    MuzaxOscillator operator_a;
    MuzaxOscillator operator_b;
    uint8_t channel_config;
    std::array<uint8_t, 5> tail;
};

struct MuzaxCommandHead {
    std::size_t index;
    uint8_t low;
    uint8_t high;
    uint8_t function_type;
    uint8_t channel;
};

struct MuzaxCommandSummary {
    std::size_t byte_length;
    std::size_t odd_trailing_byte;
    std::size_t command_count;
    std::array<std::size_t, 8> function_counts;
    std::vector<MuzaxCommandHead> head;
};

struct MuzaxSong {
    MuzaxSongHeader header;
    std::size_t next_song_start = 0;
    std::optional<std::array<uint8_t, 3>> widths;
    std::optional<std::size_t> compressed_end;
    std::optional<std::size_t> compressed_size;
    std::optional<Bytes> payload;
    std::size_t instrument_bytes = 0;
    std::size_t command_bytes = 0;
    std::vector<MuzaxInstrument> instruments;
    std::optional<Bytes> commands;
    std::optional<MuzaxCommandSummary> command_summary;

    bool is_empty() const { return header.is_empty(); }
};

struct MuzaxArchive {
    std::size_t source_len = 0;
    uint16_t song_table_size = 0;
    std::vector<MuzaxSong> songs;

    std::size_t song_count() const { return songs.size(); }
    std::size_t populated_song_count() const {
        std::size_t count = 0;
        for (const auto& song : songs) {
            if (!song.is_empty()) count += 1;
        }
        return count;
    }
};

MuzaxArchive load_muzax_lzs_bytes(const Bytes& data);
MuzaxArchive load_muzax_lzs_path(const std::string& path);

} // namespace skyroads::data
