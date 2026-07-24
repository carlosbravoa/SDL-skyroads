#include "core/dos_render_tables.hpp"

namespace skyroads::core {
namespace {

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

} // namespace

uint8_t dos_tile_class(uint8_t dispatch_variant_low3) {
    return DOS_TILE_CLASS_BY_LOW3[dispatch_variant_low3];
}

DispatchEntry dos_dispatch_entry(std::size_t dispatch_kind) {
    const uint16_t target = DOS_DRAW_DISPATCH_TARGETS[dispatch_kind];
    return DispatchEntry{dispatch_kind, target, dispatch_label(target)};
}

} // namespace skyroads::core
