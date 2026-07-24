#include "data/dashboard.hpp"

namespace skyroads::data {

HudFragmentPack load_dashboard_dat_bytes(const Bytes& data) {
    if (data.size() < 4) {
        throw Error::invalid_format(
            "dashboard DAT is too small to contain a header");
    }

    const uint16_t probe = read_u16(data, 2);
    const uint16_t header_words = (probe == 0x2C) ? 0x22 : 0x0A;
    std::size_t cursor = static_cast<std::size_t>(header_words) * 2;
    if (cursor > data.size()) {
        throw Error::invalid_format("dashboard DAT header exceeds source length");
    }

    HudFragmentPack pack;
    pack.source_len = data.size();
    pack.header_words = header_words;

    while (cursor < data.size()) {
        if (cursor + 4 > data.size()) {
            throw Error::invalid_format("dashboard DAT fragment header is truncated");
        }
        const uint16_t position = read_u16(data, cursor);
        const uint8_t width = data[cursor + 2];
        const uint8_t height = data[cursor + 3];
        cursor += 4;
        const std::size_t pixel_count =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (cursor + pixel_count > data.size()) {
            throw Error::invalid_format("dashboard DAT fragment payload is truncated");
        }
        HudFragment fragment;
        fragment.position = position;
        fragment.x = position % SCREEN_WIDTH;
        fragment.y = position / SCREEN_WIDTH;
        fragment.width = width;
        fragment.height = height;
        fragment.pixels.assign(data.begin() + cursor,
                               data.begin() + cursor + pixel_count);
        pack.fragments.push_back(std::move(fragment));
        cursor += pixel_count;
    }

    return pack;
}

HudFragmentPack load_dashboard_dat_path(const std::string& path) {
    return load_dashboard_dat_bytes(read_file(path));
}

} // namespace skyroads::data
