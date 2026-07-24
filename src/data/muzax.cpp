#include "data/muzax.hpp"

#include <utility>

#include "data/compression.hpp"

namespace skyroads::data {
namespace {

std::vector<MuzaxSongHeader> parse_song_headers(const Bytes& data,
                                                uint16_t song_table_size) {
    const std::size_t count = song_table_size / 6;
    std::vector<MuzaxSongHeader> headers;
    headers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        MuzaxSongHeader header;
        header.index = index;
        header.start_pos = read_u16(data, index * 6);
        header.num_instruments = read_u16(data, index * 6 + 2);
        header.uncompressed_length = read_u16(data, index * 6 + 4);
        headers.push_back(header);
    }
    return headers;
}

std::vector<std::size_t> build_next_song_starts(
    const std::vector<MuzaxSongHeader>& headers, std::size_t file_len) {
    std::vector<std::size_t> next_song_starts;
    next_song_starts.reserve(headers.size());
    for (std::size_t index = 0; index < headers.size(); ++index) {
        std::size_t next_start = file_len;
        for (std::size_t j = index + 1; j < headers.size(); ++j) {
            if (headers[j].start_pos != 0) {
                next_start = headers[j].start_pos;
                break;
            }
        }
        next_song_starts.push_back(next_start);
    }
    return next_song_starts;
}

MuzaxOscillator parse_oscillator(const uint8_t* block) {
    const uint8_t tremolo = block[0];
    const uint8_t key_scale_level = block[1];
    const uint8_t attack_rate = block[2];
    const uint8_t sustain_level = block[3];
    const uint8_t wave_form = block[4];
    MuzaxOscillator osc;
    osc.tremolo = (tremolo & 0x80) != 0;
    osc.vibrato = (tremolo & 0x40) != 0;
    osc.sound_sustaining = (tremolo & 0x20) != 0;
    osc.key_scaling = (tremolo & 0x10) != 0;
    osc.multiplication = tremolo & 0x0F;
    osc.key_scale_level = key_scale_level >> 6;
    osc.output_level = key_scale_level & 0x3F;
    osc.attack_rate = attack_rate >> 4;
    osc.decay_rate = attack_rate & 0x0F;
    osc.sustain_level = sustain_level >> 4;
    osc.release_rate = sustain_level & 0x0F;
    osc.wave_form = wave_form & 0x07;
    return osc;
}

std::vector<MuzaxInstrument> parse_instruments(const uint8_t* data,
                                               std::size_t data_len,
                                               std::size_t count) {
    std::vector<MuzaxInstrument> instruments;
    instruments.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t start = index * 16;
        const std::size_t end = start + 16;
        if (end > data_len) {
            throw Error::invalid_format("MUZAX instrument " +
                                        std::to_string(index) + " is truncated");
        }
        MuzaxInstrument instrument;
        instrument.index = index;
        for (std::size_t b = 0; b < 16; ++b) {
            instrument.raw[b] = data[start + b];
        }
        instrument.operator_a = parse_oscillator(data + start);
        instrument.operator_b = parse_oscillator(data + start + 5);
        instrument.channel_config = data[start + 10];
        for (std::size_t b = 0; b < 5; ++b) {
            instrument.tail[b] = data[start + 11 + b];
        }
        instruments.push_back(instrument);
    }
    return instruments;
}

MuzaxCommandSummary summarize_commands(const Bytes& data,
                                       std::size_t head_count) {
    const std::size_t command_count = data.size() / 2;
    MuzaxCommandSummary summary;
    summary.byte_length = data.size();
    summary.odd_trailing_byte = data.size() % 2;
    summary.command_count = command_count;
    summary.function_counts = {0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t index = 0; index < command_count; ++index) {
        const uint8_t low = data[index * 2];
        const uint8_t high = data[index * 2 + 1];
        const uint8_t function_type = low & 0x07;
        const uint8_t channel = low >> 4;
        summary.function_counts[function_type] += 1;
        if (summary.head.size() < head_count) {
            summary.head.push_back(
                MuzaxCommandHead{index, low, high, function_type, channel});
        }
    }
    return summary;
}

} // namespace

MuzaxArchive load_muzax_lzs_bytes(const Bytes& data) {
    const uint16_t song_table_size = read_u16(data, 0);
    if (song_table_size % 6 != 0) {
        throw Error::invalid_format(
            "MUZAX.LZS song table size is not a multiple of 6: " +
            std::to_string(song_table_size));
    }
    if (static_cast<std::size_t>(song_table_size) > data.size()) {
        throw Error::invalid_format(
            "MUZAX.LZS song table size is out of range: " +
            std::to_string(song_table_size));
    }

    const std::vector<MuzaxSongHeader> headers =
        parse_song_headers(data, song_table_size);
    const std::vector<std::size_t> next_song_starts =
        build_next_song_starts(headers, data.size());

    MuzaxArchive archive;
    archive.source_len = data.size();
    archive.song_table_size = song_table_size;
    archive.songs.reserve(headers.size());

    for (std::size_t i = 0; i < headers.size(); ++i) {
        const MuzaxSongHeader& header = headers[i];
        const std::size_t next_song_start = next_song_starts[i];

        MuzaxSong song;
        song.header = header;
        song.next_song_start = next_song_start;

        if (header.is_empty()) {
            archive.songs.push_back(std::move(song));
            continue;
        }

        const std::size_t start_pos = header.start_pos;
        if (start_pos + 3 > data.size()) {
            throw Error::invalid_format("MUZAX song " +
                                        std::to_string(header.index) +
                                        " starts out of range");
        }
        const std::array<uint8_t, 3> widths = {
            data[start_pos], data[start_pos + 1], data[start_pos + 2]};
        DecompressResult decoded = decompress_stream(
            data, start_pos + 3, header.uncompressed_length,
            CompressionWidths{widths[0], widths[1], widths[2]});
        Bytes payload = std::move(decoded.output);
        const std::size_t instrument_bytes =
            static_cast<std::size_t>(header.num_instruments) * 16;
        if (instrument_bytes > payload.size()) {
            throw Error::invalid_format("MUZAX song " +
                                        std::to_string(header.index) +
                                        " instrument region exceeds payload");
        }
        Bytes commands_blob(payload.begin() + instrument_bytes, payload.end());
        std::vector<MuzaxInstrument> instruments = parse_instruments(
            payload.data(), instrument_bytes, header.num_instruments);
        MuzaxCommandSummary command_summary =
            summarize_commands(commands_blob, 32);

        song.widths = widths;
        song.compressed_end = start_pos + 3 + decoded.consumed;
        song.compressed_size = 3 + decoded.consumed;
        song.instrument_bytes = instrument_bytes;
        song.command_bytes = commands_blob.size();
        song.instruments = std::move(instruments);
        song.commands = std::move(commands_blob);
        song.command_summary = std::move(command_summary);
        song.payload = std::move(payload);
        archive.songs.push_back(std::move(song));
    }

    return archive;
}

MuzaxArchive load_muzax_lzs_path(const std::string& path) {
    return load_muzax_lzs_bytes(read_file(path));
}

} // namespace skyroads::data
