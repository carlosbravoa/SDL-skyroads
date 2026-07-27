// Part of the SkyRoads SDL port
//
// Locating the original game data. The port ships no game files: the player supplies
// their own copy of SkyRoads, and it can live in a number of reasonable places
// depending on how the game was installed and how the port was packaged.
#pragma once

#include <string>
#include <vector>

namespace skyroads::data {

// One file the asset directory must contain for it to count as a SkyRoads install.
constexpr const char* ASSET_SENTINEL = "ROADS.LZS";

// Joins root and name, tolerating a data directory whose filenames are in a different
// case. DOS filenames are case-insensitive, so a copy of the game may have arrived
// with ROADS.LZS, roads.lzs or Roads.Lzs depending on how it was unpacked; on Linux
// those are three different files. Falls back to the literal join when nothing
// matches, so error messages still name the file that was wanted.
std::string asset_path(const std::string& root, const std::string& name);

// True when `root` looks like a SkyRoads data directory.
bool is_asset_directory(const std::string& root);

// Every directory searched for the game data, in order, given the environment. Used
// both to find the data and to tell the player where it was looked for.
std::vector<std::string> asset_search_paths();

// The first entry of asset_search_paths() that is an asset directory, or an empty
// string. `explicit_root` (a command-line argument) wins over everything when set.
std::string find_asset_directory(const std::string& explicit_root);

// Where writable state belongs. skyroads.cfg must NOT simply live beside the game
// data: that may be read-only, or shared between users, or inside a snap's read-only
// area. Under a snap this is $SNAP_USER_COMMON; otherwise it is the asset directory,
// which is what the original did.
std::string writable_state_dir(const std::string& asset_root);

} // namespace skyroads::data
