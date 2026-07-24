// Part of the SkyRoads SDL port
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

constexpr std::size_t TREKDAT_POINTER_ROWS = 13;
constexpr std::size_t TREKDAT_POINTER_COLUMNS = 24;
constexpr std::size_t TREKDAT_POINTER_COUNT =
    TREKDAT_POINTER_ROWS * TREKDAT_POINTER_COLUMNS;
constexpr std::size_t TREKDAT_POINTER_TABLE_BYTES = TREKDAT_POINTER_COUNT * 2;
constexpr std::size_t TREKDAT_DOS_DRAW_ROWS = 11;
constexpr std::size_t TREKDAT_DOS_DRAW_COLUMNS = 4;
constexpr std::size_t TREKDAT_DOS_POINTERS_PER_CELL = 6;
constexpr std::size_t TREKDAT_SHAPE_ROWS = 0x410;
constexpr uint32_t TREKDAT_SHAPE_BASE = 10240;
constexpr uint16_t TREKDAT_VIEWPORT_WIDTH = 320;
constexpr uint16_t TREKDAT_VIEWPORT_HEIGHT = 200;

struct TrekdatSpan {
    uint16_t x;
    uint16_t y;
    uint8_t width;
    uint8_t offset;
};

struct TrekdatBbox {
    uint16_t x0;
    uint16_t y0;
    uint16_t x1;
    uint16_t y1;
    uint16_t width;
    uint16_t height;
};

struct TrekdatShape {
    uint16_t start_offset;
    std::size_t size;
    uint8_t color;
    uint32_t base_ptr;
    std::size_t span_count;
    std::size_t nonzero_padding_count;
    std::optional<TrekdatBbox> bbox;
    std::vector<TrekdatSpan> spans;
};

struct TrekdatCellPointers {
    std::array<uint16_t, TREKDAT_DOS_POINTERS_PER_CELL> pointers;
};

struct TrekdatPointerRow {
    std::array<TrekdatCellPointers, TREKDAT_DOS_DRAW_COLUMNS> cells;
};

struct TrekdatDosPointerLayout {
    std::array<TrekdatPointerRow, TREKDAT_POINTER_ROWS> rows;
};

struct TrekdatRecord {
    std::size_t index;
    std::size_t file_offset;
    std::size_t next_file_offset;
    std::size_t compressed_size;
    uint16_t load_buff_end;
    uint16_t bytes_to_read;
    std::size_t load_offset;
    std::array<uint8_t, 3> widths;
    Bytes payload;
    Bytes expanded;
    std::vector<uint16_t> pointer_table;
    std::map<uint16_t, TrekdatShape> shapes;

    std::size_t unique_pointer_count() const { return shapes.size(); }
    std::size_t total_span_count() const;
    uint16_t pointer_min() const;
    uint16_t pointer_max() const;
    TrekdatDosPointerLayout dos_pointer_layout() const;
    std::optional<TrekdatShape> shape_at_offset(uint16_t start_offset) const;
    std::optional<uint16_t> next_shape_offset(uint16_t start_offset) const;
};

struct TrekdatArchive {
    std::size_t source_len;
    std::vector<TrekdatRecord> records;
    std::size_t record_count() const { return records.size(); }
};

TrekdatArchive load_trekdat_lzs_bytes(const Bytes& data);
TrekdatArchive load_trekdat_lzs_path(const std::string& path);

} // namespace skyroads::data
