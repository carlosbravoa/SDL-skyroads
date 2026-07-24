#include "data/sound.hpp"

namespace skyroads::data {

Pcm8Sample load_intro_snd_bytes(const Bytes& data) {
    return Pcm8Sample{SAMPLE_RATE_PCM_8K, data};
}

Pcm8Sample load_intro_snd_path(const std::string& path) {
    return load_intro_snd_bytes(read_file(path));
}

SfxBank load_sfx_snd_bytes(const Bytes& data) {
    if (data.size() < 2) {
        throw Error::invalid_format(
            "SFX.SND is too small to contain an offset table");
    }

    const std::size_t first_offset = read_u16(data, 0);
    if (first_offset % 2 != 0) {
        throw Error::invalid_format("SFX.SND first offset is not aligned: " +
                                    std::to_string(first_offset));
    }
    if (first_offset > data.size()) {
        throw Error::invalid_format("SFX.SND first offset is out of range: " +
                                    std::to_string(first_offset));
    }

    std::vector<std::size_t> offsets;
    offsets.reserve(first_offset / 2);
    for (std::size_t offset = 0; offset < first_offset; offset += 2) {
        offsets.push_back(read_u16(data, offset));
    }
    if ((offsets.empty() ? 0 : offsets.front()) != first_offset) {
        throw Error::invalid_format(
            "SFX.SND offset table does not point to its first payload");
    }
    for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
        if (offsets[i] > offsets[i + 1]) {
            throw Error::invalid_format(
                "SFX.SND offsets are not monotonically increasing");
        }
    }

    SfxBank bank;
    bank.source_len = data.size();
    bank.sample_rate = SAMPLE_RATE_PCM_8K;
    bank.effects.reserve(offsets.size());
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        const std::size_t start = offsets[index];
        const std::size_t end =
            (index + 1 < offsets.size()) ? offsets[index + 1] : data.size();
        if (end > data.size() || start > end) {
            throw Error::invalid_format("SFX.SND effect " +
                                        std::to_string(index) +
                                        " has invalid range");
        }
        SfxEntry entry;
        entry.index = index;
        entry.start = start;
        entry.end = end;
        entry.sample.sample_rate = SAMPLE_RATE_PCM_8K;
        entry.sample.samples.assign(data.begin() + start, data.begin() + end);
        bank.effects.push_back(std::move(entry));
    }
    return bank;
}

SfxBank load_sfx_snd_path(const std::string& path) {
    return load_sfx_snd_bytes(read_file(path));
}

} // namespace skyroads::data
