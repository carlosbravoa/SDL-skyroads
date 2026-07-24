#include "data/exe.hpp"

#include <cstring>

namespace skyroads::data {
namespace {

constexpr uint16_t RUNTIME_TILE_CLASS_OFFSET = 0x0B77;
constexpr uint16_t RUNTIME_DISPATCH_OFFSET = 0x0B7F;

std::optional<std::string> dispatch_label(uint16_t target) {
    switch (target) {
        case 0x2E50: return std::string("draw_type_0");
        case 0x303D: return std::string("draw_type_1");
        case 0x2E9F: return std::string("draw_type_2");
        case 0x2EE1: return std::string("draw_type_3");
        case 0x2F3C: return std::string("draw_type_4");
        case 0x2FB0: return std::string("draw_type_5");
        case 0x3AAD: return std::string("noop");
        default: return std::nullopt;
    }
}

ExeRuntimeTables extract_runtime_tables(const Bytes& data,
                                        std::size_t header_bytes,
                                        std::size_t exe_reader_base_file_offset) {
    ExeRuntimeTables tables;

    {
        const uint16_t offset = RUNTIME_TILE_CLASS_OFFSET;
        const std::size_t file_offset = exe_reader_base_file_offset + offset;
        const std::size_t end = file_offset + 8;
        if (end > data.size()) {
            throw Error::invalid_format(
                "tile_class_by_low3 table extends past SKYROADS.EXE");
        }
        tables.tile_class_by_low3.offset = offset;
        tables.tile_class_by_low3.image_offset = file_offset - header_bytes;
        tables.tile_class_by_low3.file_offset = file_offset;
        tables.tile_class_by_low3.values.assign(data.begin() + file_offset,
                                                data.begin() + end);
    }

    {
        const uint16_t offset = RUNTIME_DISPATCH_OFFSET;
        const std::size_t file_offset = exe_reader_base_file_offset + offset;
        const std::size_t end = file_offset + (16 * 2);
        if (end > data.size()) {
            throw Error::invalid_format(
                "draw_dispatch_by_type table extends past SKYROADS.EXE");
        }
        tables.draw_dispatch_by_type.offset = offset;
        tables.draw_dispatch_by_type.image_offset = file_offset - header_bytes;
        tables.draw_dispatch_by_type.file_offset = file_offset;
        for (std::size_t index = 0; index < 16; ++index) {
            const uint16_t target = read_u16(data, file_offset + index * 2);
            tables.draw_dispatch_by_type.entries.push_back(
                ExeDispatchEntry{index, target, dispatch_label(target)});
        }
    }

    return tables;
}

} // namespace

SkyroadsExe load_skyroads_exe_bytes(const Bytes& data) {
    if (data.size() < 28 || data[0] != 'M' || data[1] != 'Z') {
        throw Error::invalid_format(
            "SKYROADS.EXE is not a recognized MZ executable");
    }

    const uint16_t last_page_bytes = read_u16(data, 2);
    const uint16_t pages = read_u16(data, 4);
    const uint16_t relocation_count = read_u16(data, 6);
    const uint16_t header_paragraphs = read_u16(data, 8);
    const uint16_t min_alloc = read_u16(data, 10);
    const uint16_t max_alloc = read_u16(data, 12);
    const uint16_t ss = read_u16(data, 14);
    const uint16_t sp = read_u16(data, 16);
    const uint16_t checksum = read_u16(data, 18);
    const uint16_t ip = read_u16(data, 20);
    const uint16_t cs = read_u16(data, 22);
    const uint16_t relocation_table_offset = read_u16(data, 24);
    const uint16_t overlay = read_u16(data, 26);

    const std::size_t declared_file_size =
        (static_cast<std::size_t>(pages) - 1) * 512 +
        (last_page_bytes == 0 ? 512 : last_page_bytes);
    const std::size_t header_bytes = static_cast<std::size_t>(header_paragraphs) * 16;
    if (declared_file_size > data.size()) {
        throw Error::invalid_format("SKYROADS.EXE header declares more bytes "
                                    "than the file contains");
    }
    if (header_bytes > declared_file_size) {
        throw Error::invalid_format(
            "SKYROADS.EXE header is larger than declared file size");
    }
    const std::size_t image_size = declared_file_size - header_bytes;

    SkyroadsExe exe;
    exe.declared_file_size = declared_file_size;
    exe.header_bytes = header_bytes;
    exe.image_size = image_size;
    exe.relocation_count = relocation_count;
    exe.min_alloc = min_alloc;
    exe.max_alloc = max_alloc;
    exe.ss = ss;
    exe.sp = sp;
    exe.checksum = checksum;
    exe.ip = ip;
    exe.cs = cs;
    exe.relocation_table_offset = relocation_table_offset;
    exe.overlay = overlay;
    exe.image.assign(data.begin() + header_bytes,
                     data.begin() + header_bytes + image_size);

    exe.relocations.reserve(relocation_count);
    for (std::size_t index = 0; index < relocation_count; ++index) {
        const std::size_t entry_offset =
            static_cast<std::size_t>(relocation_table_offset) + index * 4;
        if (entry_offset + 4 > data.size()) {
            throw Error::invalid_format("SKYROADS.EXE relocation " +
                                        std::to_string(index) + " is truncated");
        }
        const uint16_t offset = read_u16(data, entry_offset);
        const uint16_t segment = read_u16(data, entry_offset + 2);
        const std::size_t image_offset =
            static_cast<std::size_t>(segment) * 16 + offset;
        exe.relocations.push_back(ExeRelocation{index, offset, segment,
                                                image_offset,
                                                header_bytes + image_offset});
    }

    exe.entry_image_offset = static_cast<std::size_t>(cs) * 16 + ip;
    exe.entry_file_offset = header_bytes + exe.entry_image_offset;
    exe.exe_reader_base_image_offset =
        static_cast<std::size_t>(EXE_READER_SEGMENT_BASE) * 16;
    exe.exe_reader_base_file_offset =
        header_bytes + exe.exe_reader_base_image_offset;
    exe.runtime_tables =
        extract_runtime_tables(data, header_bytes, exe.exe_reader_base_file_offset);

    return exe;
}

SkyroadsExe load_skyroads_exe_path(const std::string& path) {
    return load_skyroads_exe_bytes(read_file(path));
}

} // namespace skyroads::data
