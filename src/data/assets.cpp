#include "data/assets.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>

namespace skyroads::data {
namespace {

std::string lowered(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool file_exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

void add_if_set(std::vector<std::string>& out, const std::string& base,
                const std::string& suffix) {
    if (base.empty()) return;
    out.push_back(suffix.empty() ? base : base + "/" + suffix);
}

} // namespace

std::string asset_path(const std::string& root, const std::string& name) {
    const std::string direct = root + "/" + name;
    if (file_exists(direct)) return direct;

    // Nothing at the exact name: look for a case-insensitive match.
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) return direct;
    const std::string wanted = lowered(name);
    std::string found;
    while (dirent* entry = readdir(dir)) {
        if (lowered(entry->d_name) == wanted) {
            found = root + "/" + entry->d_name;
            break;
        }
    }
    closedir(dir);
    return found.empty() ? direct : found;
}

bool is_asset_directory(const std::string& root) {
    if (root.empty()) return false;
    return file_exists(asset_path(root, ASSET_SENTINEL));
}

std::vector<std::string> asset_search_paths() {
    std::vector<std::string> paths;
    // An explicit environment override always comes first.
    add_if_set(paths, env_or_empty("SKYROADS_ASSETS"), "");
    // Inside a snap, the writable per-user area is always reachable with no
    // interfaces connected, so it is the natural drop spot.
    add_if_set(paths, env_or_empty("SNAP_USER_COMMON"), "gamedata");
    // The conventional spots, in the casings people actually use.
    const std::string home = env_or_empty("HOME");
    for (const char* candidate : {"Games/SkyRoads", "Games/skyroads",
                                  "games/SkyRoads", "games/skyroads",
                                  "SkyRoads", "skyroads"}) {
        add_if_set(paths, home, candidate);
    }
    // Finally the working directory, which is how it has always been run in-tree.
    paths.push_back(".");
    paths.push_back("skyroads-assets");
    return paths;
}

std::string find_asset_directory(const std::string& explicit_root) {
    if (!explicit_root.empty()) {
        return is_asset_directory(explicit_root) ? explicit_root : std::string();
    }
    for (const std::string& candidate : asset_search_paths()) {
        if (is_asset_directory(candidate)) return candidate;
    }
    return std::string();
}

std::string writable_state_dir(const std::string& asset_root) {
    const std::string snap_common = env_or_empty("SNAP_USER_COMMON");
    if (!snap_common.empty()) return snap_common;
    return asset_root;
}

} // namespace skyroads::data
