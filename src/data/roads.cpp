#include "data/roads.hpp"

#include <set>

#include "data/compression.hpp"

namespace skyroads::data {
namespace {

RoadDescriptorCatalog build_road_descriptor_catalog(
    const std::vector<RoadEntry>& roads) {
    struct DescriptorAccum {
        std::size_t count = 0;
        std::set<std::size_t> roads;
        std::vector<RoadSample> samples;
    };
    struct DispatchAccum {
        std::size_t count = 0;
        std::set<std::size_t> roads;
        std::set<uint16_t> descriptors;
        std::vector<DispatchSample> samples;
    };

    // std::map iterates in sorted key order, matching the reference's BTreeMap so the
    // emitted catalog ordering is identical.
    std::map<uint16_t, DescriptorAccum> descriptor_counts;
    std::map<uint8_t, DispatchAccum> dispatch_counts;

    for (const RoadEntry& road : roads) {
        for (std::size_t row_index = 0; row_index < road.rows.size();
             ++row_index) {
            const RoadRow& row = road.rows[row_index];
            for (std::size_t column_index = 0; column_index < row.size();
                 ++column_index) {
                const uint16_t value = row[column_index];
                const RoadDescriptor analyzed = analyze_road_descriptor(value);

                DescriptorAccum& d = descriptor_counts[value];
                d.count += 1;
                d.roads.insert(road.index);
                if (d.samples.size() < 8) {
                    d.samples.push_back(
                        RoadSample{road.index, row_index, column_index});
                }

                DispatchAccum& k = dispatch_counts[analyzed.dispatch_kind];
                k.count += 1;
                k.roads.insert(road.index);
                k.descriptors.insert(value);
                if (k.samples.size() < 8) {
                    k.samples.push_back(DispatchSample{
                        road.index, row_index, column_index, value});
                }
            }
        }
    }

    RoadDescriptorCatalog catalog;
    for (auto& [raw, accum] : descriptor_counts) {
        RoadDescriptorEntry entry;
        entry.descriptor = analyze_road_descriptor(raw);
        entry.count = accum.count;
        entry.roads.assign(accum.roads.begin(), accum.roads.end());
        entry.samples = std::move(accum.samples);
        catalog.descriptors.push_back(std::move(entry));
    }
    for (auto& [dispatch_kind, accum] : dispatch_counts) {
        DispatchKindEntry entry;
        entry.dispatch_kind = dispatch_kind;
        entry.count = accum.count;
        entry.roads.assign(accum.roads.begin(), accum.roads.end());
        entry.descriptor_count = accum.descriptors.size();
        entry.descriptors.assign(accum.descriptors.begin(),
                                 accum.descriptors.end());
        entry.samples = std::move(accum.samples);
        catalog.dispatch_kinds.push_back(std::move(entry));
    }
    for (const auto& entry : catalog.dispatch_kinds) {
        catalog.used_dispatch_kinds.push_back(entry.dispatch_kind);
    }
    return catalog;
}

} // namespace

RoadDescriptor analyze_road_descriptor(uint16_t value) {
    const uint8_t low_byte = static_cast<uint8_t>(value & 0x00FF);
    const uint8_t high_byte = static_cast<uint8_t>(value >> 8);
    RoadDescriptor d;
    d.raw = value;
    d.low_byte = low_byte;
    d.high_byte = high_byte;
    d.dispatch_kind = high_byte & 0x0F;
    d.dispatch_variant_low3 = high_byte & 0x07;
    d.high_flags = high_byte >> 4;
    return d;
}

RoadsArchive load_roads_lzs_bytes(const Bytes& data) {
    const uint16_t first_offset = read_u16(data, 0);
    if (first_offset % 4 != 0) {
        throw Error::invalid_format("unexpected ROADS header size " +
                                    std::to_string(first_offset));
    }

    const std::size_t entry_count = first_offset / 4;
    std::vector<std::pair<uint16_t, uint16_t>> entries;
    entries.reserve(entry_count);
    for (std::size_t index = 0; index < entry_count; ++index) {
        const uint16_t offset = read_u16(data, index * 4);
        const uint16_t unpacked_size = read_u16(data, index * 4 + 2);
        entries.emplace_back(offset, unpacked_size);
    }

    std::vector<RoadEntry> roads;
    roads.reserve(entry_count);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::size_t offset = entries[index].first;
        const uint16_t unpacked_size = entries[index].second;
        const std::size_t next_offset =
            (index + 1 < entries.size()) ? entries[index + 1].first : data.size();
        if (next_offset < offset || next_offset > data.size()) {
            throw Error::invalid_format("road " + std::to_string(index) +
                                        " has invalid slice bounds");
        }

        const Bytes road_blob(data.begin() + offset, data.begin() + next_offset);
        if (road_blob.size() < 225) {
            throw Error::invalid_format(
                "road " + std::to_string(index) +
                " is too small to contain metadata and compression widths");
        }

        const uint16_t gravity = read_u16(road_blob, 0);
        const uint16_t fuel = read_u16(road_blob, 2);
        const uint16_t oxygen = read_u16(road_blob, 4);
        Bytes palette_vga(road_blob.begin() + 6, road_blob.begin() + 222);
        const std::array<uint8_t, 3> widths = {road_blob[222], road_blob[223],
                                               road_blob[224]};
        DecompressResult decoded = decompress_stream(
            road_blob, 225, unpacked_size,
            CompressionWidths{widths[0], widths[1], widths[2]});
        Bytes raw_tiles = std::move(decoded.output);

        if (raw_tiles.size() % 2 != 0) {
            throw Error::invalid_format("road " + std::to_string(index) +
                                        " decompressed to an odd number of bytes");
        }

        std::vector<uint16_t> values;
        values.reserve(raw_tiles.size() / 2);
        for (std::size_t i = 0; i + 1 < raw_tiles.size(); i += 2) {
            values.push_back(static_cast<uint16_t>(raw_tiles[i]) |
                             (static_cast<uint16_t>(raw_tiles[i + 1]) << 8));
        }
        if (values.size() % ROAD_COLUMNS != 0) {
            throw Error::invalid_format(
                "road " + std::to_string(index) + " decompressed to " +
                std::to_string(values.size()) + " cells, not a multiple of 7");
        }

        RoadEntry road;
        road.index = index;
        road.offset = static_cast<uint16_t>(offset);
        road.compressed_size = road_blob.size();
        road.unpacked_size = unpacked_size;
        road.gravity = gravity;
        road.fuel = fuel;
        road.oxygen = oxygen;
        road.palette_vga = std::move(palette_vga);
        road.widths = widths;

        road.rows.reserve(values.size() / ROAD_COLUMNS);
        for (std::size_t base = 0; base < values.size(); base += ROAD_COLUMNS) {
            RoadRow r;
            for (std::size_t c = 0; c < ROAD_COLUMNS; ++c) {
                r[c] = values[base + c];
            }
            road.rows.push_back(r);
        }

        std::set<uint16_t> descriptors;
        for (uint16_t value : values) {
            const RoadDescriptor analyzed = analyze_road_descriptor(value);
            road.dispatch_kind_counts[analyzed.dispatch_kind] += 1;
            descriptors.insert(value);
        }
        road.raw_tiles = std::move(raw_tiles);
        road.descriptor_count = descriptors.size();

        roads.push_back(std::move(road));
    }

    RoadsArchive archive;
    archive.source_len = data.size();
    archive.descriptor_catalog = build_road_descriptor_catalog(roads);
    archive.roads = std::move(roads);
    return archive;
}

RoadsArchive load_roads_lzs_path(const std::string& path) {
    return load_roads_lzs_bytes(read_file(path));
}

} // namespace skyroads::data
