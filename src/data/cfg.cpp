#include "data/cfg.hpp"

namespace skyroads::data {

SkyroadsCfg load_cfg_bytes(const Bytes& data) { return SkyroadsCfg{data}; }

SkyroadsCfg load_cfg_path(const std::string& path) {
    return load_cfg_bytes(read_file(path));
}

} // namespace skyroads::data
