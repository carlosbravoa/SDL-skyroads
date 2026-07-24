// Part of the SkyRoads SDL port
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

constexpr uint16_t EXE_READER_SEGMENT_BASE = 0x66E;

struct ExeRelocation {
    std::size_t index;
    uint16_t offset;
    uint16_t segment;
    std::size_t image_offset;
    std::size_t file_offset;
};

struct ExeRuntimeU8Table {
    uint16_t offset;
    std::size_t image_offset;
    std::size_t file_offset;
    Bytes values;
};

struct ExeDispatchEntry {
    std::size_t index;
    uint16_t target;
    // the reference design: Option<&'static str>. Empty string models None here.
    std::optional<std::string> target_label;
};

struct ExeRuntimeDispatchTable {
    uint16_t offset;
    std::size_t image_offset;
    std::size_t file_offset;
    std::vector<ExeDispatchEntry> entries;
};

struct ExeRuntimeTables {
    ExeRuntimeU8Table tile_class_by_low3;
    ExeRuntimeDispatchTable draw_dispatch_by_type;
};

struct SkyroadsExe {
    std::size_t declared_file_size;
    std::size_t header_bytes;
    std::size_t image_size;
    uint16_t relocation_count;
    uint16_t min_alloc;
    uint16_t max_alloc;
    uint16_t ss;
    uint16_t sp;
    uint16_t checksum;
    uint16_t ip;
    uint16_t cs;
    uint16_t relocation_table_offset;
    uint16_t overlay;
    std::vector<ExeRelocation> relocations;
    std::size_t entry_image_offset;
    std::size_t entry_file_offset;
    std::size_t exe_reader_base_image_offset;
    std::size_t exe_reader_base_file_offset;
    Bytes image;
    ExeRuntimeTables runtime_tables;
};

SkyroadsExe load_skyroads_exe_bytes(const Bytes& data);
SkyroadsExe load_skyroads_exe_path(const std::string& path);

} // namespace skyroads::data
