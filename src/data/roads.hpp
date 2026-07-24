// Part of the SkyRoads SDL port
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

constexpr std::size_t ROAD_COLUMNS = 7;
using RoadRow = std::array<uint16_t, ROAD_COLUMNS>;

struct RoadDescriptor {
    uint16_t raw;
    uint8_t low_byte;
    uint8_t high_byte;
    uint8_t dispatch_kind;
    uint8_t dispatch_variant_low3;
    uint8_t high_flags;
};

struct RoadSample {
    std::size_t road_index;
    std::size_t row_index;
    std::size_t column_index;
};

struct DispatchSample {
    std::size_t road_index;
    std::size_t row_index;
    std::size_t column_index;
    uint16_t raw;
};

struct RoadDescriptorEntry {
    RoadDescriptor descriptor;
    std::size_t count;
    std::vector<std::size_t> roads;
    std::vector<RoadSample> samples;
};

struct DispatchKindEntry {
    uint8_t dispatch_kind;
    std::size_t count;
    std::vector<std::size_t> roads;
    std::size_t descriptor_count;
    std::vector<uint16_t> descriptors;
    std::vector<DispatchSample> samples;
};

struct RoadDescriptorCatalog {
    std::vector<uint8_t> used_dispatch_kinds;
    std::vector<DispatchKindEntry> dispatch_kinds;
    std::vector<RoadDescriptorEntry> descriptors;
};

struct RoadEntry {
    std::size_t index;
    uint16_t offset;
    std::size_t compressed_size;
    uint16_t unpacked_size;
    uint16_t gravity;
    uint16_t fuel;
    uint16_t oxygen;
    Bytes palette_vga;
    std::array<uint8_t, 3> widths;
    Bytes raw_tiles;
    std::vector<RoadRow> rows;
    std::map<uint8_t, std::size_t> dispatch_kind_counts;
    std::size_t descriptor_count;
};

struct RoadsArchive {
    std::size_t source_len;
    std::vector<RoadEntry> roads;
    RoadDescriptorCatalog descriptor_catalog;

    std::size_t road_count() const { return roads.size(); }
    const std::vector<uint8_t>& used_dispatch_kinds() const {
        return descriptor_catalog.used_dispatch_kinds;
    }
    std::size_t distinct_descriptor_count() const {
        return descriptor_catalog.descriptors.size();
    }
};

RoadDescriptor analyze_road_descriptor(uint16_t value);
RoadsArchive load_roads_lzs_bytes(const Bytes& data);
RoadsArchive load_roads_lzs_path(const std::string& path);

} // namespace skyroads::data
