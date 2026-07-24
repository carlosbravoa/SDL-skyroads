#include "data/byteio.hpp"

#include <cstdio>

namespace skyroads::data {

Bytes read_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        throw Error::io("failed to open " + path);
    }
    Bytes data;
    uint8_t buffer[65536];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.insert(data.end(), buffer, buffer + read);
    }
    const bool had_error = std::ferror(file) != 0;
    std::fclose(file);
    if (had_error) {
        throw Error::io("failed to read " + path);
    }
    return data;
}

} // namespace skyroads::data
