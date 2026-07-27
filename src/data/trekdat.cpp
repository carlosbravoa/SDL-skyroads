#include "data/trekdat.hpp"

#include <algorithm>
#include <utility>

#include "data/compression.hpp"

namespace skyroads::data {
namespace {

std::pair<Bytes, std::size_t> expand_trekdat_record(uint16_t load_buff_end_u16,
                                                    const Bytes& payload) {
    const std::size_t load_buff_end = load_buff_end_u16;
    if (payload.size() > load_buff_end) {
        throw Error::invalid_format(
            "TREKDAT record expands to a negative load offset");
    }
    const std::size_t load_offset = load_buff_end - payload.size();

    Bytes working(load_offset, 0);
    working.insert(working.end(), payload.begin(), payload.end());
    std::size_t src_ptr = load_offset;
    Bytes output;
    output.reserve(load_buff_end);

    if (src_ptr + TREKDAT_POINTER_TABLE_BYTES > working.size()) {
        throw Error::invalid_format(
            "TREKDAT record is too short for its pointer table");
    }
    output.insert(output.end(), working.begin() + src_ptr,
                  working.begin() + src_ptr + TREKDAT_POINTER_TABLE_BYTES);
    src_ptr += TREKDAT_POINTER_TABLE_BYTES;

    for (std::size_t row = 0; row < TREKDAT_SHAPE_ROWS; ++row) {
        if (src_ptr + 3 > working.size()) {
            throw Error::invalid_format(
                "TREKDAT record ended while copying shape headers");
        }
        output.insert(output.end(), working.begin() + src_ptr,
                      working.begin() + src_ptr + 3);
        src_ptr += 3;

        while (true) {
            if (src_ptr >= working.size()) {
                throw Error::invalid_format(
                    "TREKDAT record ended while copying shape spans");
            }
            const uint8_t value = working[src_ptr];
            output.push_back(value);
            src_ptr += 1;
            if (value == 0xFF) {
                break;
            }
            if (src_ptr >= working.size()) {
                throw Error::invalid_format(
                    "TREKDAT record ended while copying span width");
            }
            output.push_back(working[src_ptr]);
            src_ptr += 1;
            output.push_back(0);
        }
    }

    if (output.size() != load_buff_end) {
        throw Error::invalid_format("TREKDAT expanded size mismatch");
    }
    return {std::move(output), load_offset};
}

std::vector<uint16_t> parse_pointer_table(const Bytes& expanded,
                                          std::size_t record_index) {
    std::vector<uint16_t> pointer_table;
    pointer_table.reserve(TREKDAT_POINTER_COUNT);
    for (std::size_t entry_offset = 0;
         entry_offset < TREKDAT_POINTER_TABLE_BYTES; entry_offset += 2) {
        const uint16_t pointer = read_u16(expanded, entry_offset);
        if (static_cast<std::size_t>(pointer) < TREKDAT_POINTER_TABLE_BYTES) {
            throw Error::invalid_format(
                "TREKDAT record " + std::to_string(record_index) +
                " contains a pointer into the table area");
        }
        if (static_cast<std::size_t>(pointer) >= expanded.size()) {
            throw Error::invalid_format(
                "TREKDAT record " + std::to_string(record_index) +
                " contains an out-of-range pointer");
        }
        pointer_table.push_back(pointer);
    }
    return pointer_table;
}

TrekdatShape parse_trekdat_shape(const Bytes& expanded, uint16_t start_offset) {
    const std::size_t start = start_offset;
    if (start + 3 > expanded.size()) {
        throw Error::invalid_format("TREKDAT shape offset is out of range");
    }

    const uint8_t color = expanded[start];
    const uint32_t base_ptr = TREKDAT_SHAPE_BASE + read_u16(expanded, start + 1);
    std::size_t cursor = start + 3;
    uint32_t ptr = base_ptr;
    std::vector<TrekdatSpan> spans;
    uint16_t min_x = TREKDAT_VIEWPORT_WIDTH;
    uint16_t max_x = 0;
    uint16_t min_y = TREKDAT_VIEWPORT_HEIGHT;
    uint16_t max_y = 0;
    bool saw_any_span = false;
    std::size_t nonzero_padding_count = 0;

    while (true) {
        if (cursor >= expanded.size()) {
            throw Error::invalid_format("TREKDAT shape is truncated");
        }
        const uint8_t offset = expanded[cursor];
        cursor += 1;
        if (offset == 0xFF) {
            break;
        }
        if (cursor + 2 > expanded.size()) {
            throw Error::invalid_format("TREKDAT span is truncated");
        }
        const uint8_t width = expanded[cursor];
        const uint8_t padding = expanded[cursor + 1];
        cursor += 2;
        if (padding != 0) {
            nonzero_padding_count += 1;
        }
        if (ptr < static_cast<uint32_t>(offset)) {
            throw Error::invalid_format("TREKDAT span underflow");
        }
        const uint32_t ptr2 = ptr - static_cast<uint32_t>(offset);
        const uint16_t x = static_cast<uint16_t>(ptr2 % TREKDAT_VIEWPORT_WIDTH);
        const uint16_t y = static_cast<uint16_t>(ptr2 / TREKDAT_VIEWPORT_WIDTH);
        spans.push_back(TrekdatSpan{x, y, width, offset});
        if (width > 0) {
            const uint16_t x1 = static_cast<uint16_t>(x + width - 1);
            if (!saw_any_span) {
                min_x = x;
                max_x = x1;
                min_y = y;
                max_y = y;
                saw_any_span = true;
            } else {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x1);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
        ptr += TREKDAT_VIEWPORT_WIDTH;
    }

    TrekdatShape shape;
    shape.start_offset = start_offset;
    shape.size = cursor - start;
    shape.color = color;
    shape.base_ptr = base_ptr;
    shape.span_count = spans.size();
    shape.nonzero_padding_count = nonzero_padding_count;
    if (saw_any_span) {
        shape.bbox = TrekdatBbox{min_x, min_y, max_x, max_y,
                                 static_cast<uint16_t>(max_x - min_x + 1),
                                 static_cast<uint16_t>(max_y - min_y + 1)};
    }
    shape.spans = std::move(spans);
    return shape;
}

} // namespace

std::size_t TrekdatRecord::total_span_count() const {
    std::size_t total = 0;
    for (const auto& [key, shape] : shapes) {
        total += shape.span_count;
    }
    return total;
}

uint16_t TrekdatRecord::pointer_min() const {
    uint16_t result = 0;
    bool first = true;
    for (uint16_t p : pointer_table) {
        if (first || p < result) {
            result = p;
            first = false;
        }
    }
    return result;
}

uint16_t TrekdatRecord::pointer_max() const {
    uint16_t result = 0;
    for (uint16_t p : pointer_table) {
        if (p > result) result = p;
    }
    return result;
}

TrekdatDosPointerLayout TrekdatRecord::dos_pointer_layout() const {
    TrekdatDosPointerLayout layout;
    for (std::size_t row_index = 0; row_index < TREKDAT_POINTER_ROWS;
         ++row_index) {
        const std::size_t row_start = row_index * TREKDAT_POINTER_COLUMNS;
        TrekdatPointerRow& row = layout.rows[row_index];
        for (std::size_t cell_index = 0; cell_index < TREKDAT_DOS_DRAW_COLUMNS;
             ++cell_index) {
            const std::size_t cell_start =
                cell_index * TREKDAT_DOS_POINTERS_PER_CELL;
            for (std::size_t p = 0; p < TREKDAT_DOS_POINTERS_PER_CELL; ++p) {
                row.cells[cell_index].pointers[p] =
                    pointer_table[row_start + cell_start + p];
            }
        }
    }
    return layout;
}

std::optional<TrekdatShape> TrekdatRecord::shape_at_offset(
    uint16_t start_offset) const {
    try {
        return parse_trekdat_shape(expanded, start_offset);
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::optional<uint16_t> TrekdatRecord::next_shape_offset(
    uint16_t start_offset) const {
    const auto shape = shape_at_offset(start_offset);
    if (!shape) return std::nullopt;
    const std::size_t next_offset =
        static_cast<std::size_t>(shape->start_offset) + shape->size;
    if (next_offset + 3 > expanded.size()) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(next_offset);
}

// RVSTACK: see the header note — decode each offset once, ever.
const TrekdatShape* TrekdatRecord::shape_ref(uint16_t start_offset) const {
    const auto hit = shapes.find(start_offset);
    if (hit != shapes.end()) return &hit->second;
    const auto memo = shape_memo.find(start_offset);
    if (memo != shape_memo.end()) return &memo->second;
    try {
        const auto inserted =
            shape_memo.emplace(start_offset, parse_trekdat_shape(expanded, start_offset));
        return &inserted.first->second;
    } catch (const Error&) {
        return nullptr;
    }
}

std::optional<uint16_t> TrekdatRecord::next_shape_offset_fast(
    uint16_t start_offset) const {
    const TrekdatShape* shape = shape_ref(start_offset);
    if (!shape) return std::nullopt;
    const std::size_t next_offset =
        static_cast<std::size_t>(shape->start_offset) + shape->size;
    if (next_offset + 3 > expanded.size()) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(next_offset);
}

TrekdatArchive load_trekdat_lzs_bytes(const Bytes& data) {
    if (data.size() < 7) {
        throw Error::invalid_format(
            "TREKDAT.LZS is too small to contain the observed TREKDAT header");
    }

    std::vector<TrekdatRecord> records;
    std::size_t offset = 0;
    std::size_t record_index = 0;

    while (offset < data.size()) {
        if (offset + 7 > data.size()) {
            throw Error::invalid_format("TREKDAT record " +
                                        std::to_string(record_index) +
                                        " is truncated");
        }
        const uint16_t load_buff_end = read_u16(data, offset);
        const uint16_t bytes_to_read = read_u16(data, offset + 2);
        const std::array<uint8_t, 3> widths = {data[offset + 4], data[offset + 5],
                                               data[offset + 6]};
        DecompressResult decoded = decompress_stream(
            data, offset + 7, bytes_to_read,
            CompressionWidths{widths[0], widths[1], widths[2]});
        auto [expanded, load_offset] =
            expand_trekdat_record(load_buff_end, decoded.output);
        std::vector<uint16_t> pointer_table =
            parse_pointer_table(expanded, record_index);
        std::map<uint16_t, TrekdatShape> shapes;
        for (uint16_t start_offset : pointer_table) {
            if (shapes.find(start_offset) == shapes.end()) {
                shapes.emplace(start_offset,
                               parse_trekdat_shape(expanded, start_offset));
            }
        }
        const std::size_t next_file_offset = offset + 7 + decoded.consumed;

        TrekdatRecord record;
        record.index = record_index;
        record.file_offset = offset;
        record.next_file_offset = next_file_offset;
        record.compressed_size = next_file_offset - offset;
        record.load_buff_end = load_buff_end;
        record.bytes_to_read = bytes_to_read;
        record.load_offset = load_offset;
        record.widths = widths;
        record.payload = std::move(decoded.output);
        record.expanded = std::move(expanded);
        record.pointer_table = std::move(pointer_table);
        record.shapes = std::move(shapes);
        records.push_back(std::move(record));

        offset = next_file_offset;
        record_index += 1;
    }

    TrekdatArchive archive;
    archive.source_len = data.size();
    archive.records = std::move(records);
    return archive;
}

TrekdatArchive load_trekdat_lzs_path(const std::string& path) {
    return load_trekdat_lzs_bytes(read_file(path));
}

} // namespace skyroads::data
