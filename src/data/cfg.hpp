// Part of the SkyRoads SDL port
#pragma once

#include <string>

#include "data/byteio.hpp"

namespace skyroads::data {

struct SkyroadsCfg {
    Bytes raw;
    std::size_t byte_count() const { return raw.size(); }
};

SkyroadsCfg load_cfg_bytes(const Bytes& data);
SkyroadsCfg load_cfg_path(const std::string& path);

} // namespace skyroads::data
