// Equivalence tests for the data layer. These mirror the unit tests in
// the data layer and assert the same DOS-derived numbers, so a passing run means
// the C++ loaders agree with the reference reference byte-for-byte on the shipped
// assets.
#include <vector>

#include "check.hpp"
#include "data/data.hpp"
#include "data/config.hpp"

using namespace skyroads::data;

static void test_compression() {
    const Bytes data = {0xD0, 0x40};
    DecompressResult r = decompress_stream(data, 0, std::nullopt,
                                           CompressionWidths{4, 10, 13});
    CHECK_EQ(r.output, (Bytes{0x41}));
    CHECK_EQ(r.consumed, static_cast<std::size_t>(2));
}

static void test_image() {
    ImageArchive intro = load_image_archive_path(check::asset("INTRO.LZS"));
    CHECK_TRUE(intro.kind == ImageArchiveKind::image_set());
    CHECK_EQ(intro.frame_count(), static_cast<std::size_t>(10));
    CHECK_EQ(intro.total_fragment_count(), static_cast<std::size_t>(10));

    ImageArchive anim = load_image_archive_path(check::asset("ANIM.LZS"));
    CHECK_TRUE(anim.kind == ImageArchiveKind::animation(100));
    CHECK_EQ(anim.frame_count(), static_cast<std::size_t>(100));
    CHECK_EQ(anim.total_fragment_count(), static_cast<std::size_t>(221));

    ImageArchive main_menu = load_image_archive_path(check::asset("MAINMENU.LZS"));
    CHECK_EQ(main_menu.frame_count(), static_cast<std::size_t>(3));
}

static void test_sound() {
    Pcm8Sample intro = load_intro_snd_path(check::asset("INTRO.SND"));
    CHECK_EQ(intro.sample_rate, static_cast<uint32_t>(8000));
    CHECK_EQ(intro.sample_count(), static_cast<std::size_t>(32100));

    SfxBank sfx = load_sfx_snd_path(check::asset("SFX.SND"));
    CHECK_EQ(sfx.effect_count(), static_cast<std::size_t>(6));
    std::vector<std::size_t> lengths;
    for (const auto& e : sfx.effects) lengths.push_back(e.sample.sample_count());
    CHECK_EQ(lengths, (std::vector<std::size_t>{3984, 5154, 8085, 801, 7771, 0}));
}

static void test_dashboard() {
    HudFragmentPack oxy = load_dashboard_dat_path(check::asset("OXY_DISP.DAT"));
    CHECK_EQ(oxy.header_words, static_cast<uint16_t>(10));
    CHECK_EQ(oxy.fragment_count(), static_cast<std::size_t>(10));

    HudFragmentPack fuel = load_dashboard_dat_path(check::asset("FUL_DISP.DAT"));
    CHECK_EQ(fuel.header_words, static_cast<uint16_t>(10));
    CHECK_EQ(fuel.fragment_count(), static_cast<std::size_t>(10));

    HudFragmentPack speed = load_dashboard_dat_path(check::asset("SPEED.DAT"));
    CHECK_EQ(speed.header_words, static_cast<uint16_t>(34));
    CHECK_EQ(speed.fragment_count(), static_cast<std::size_t>(34));
}

static void test_demo() {
    DemoRecording demo = load_demo_rec_path(check::asset("DEMO.REC"));
    CHECK_EQ(demo.byte_count(), static_cast<std::size_t>(6398));
    CHECK_EQ(demo.approx_tile_length_fp16(), static_cast<uint32_t>(10479924));
    CHECK_EQ(demo.accelerate_decelerate_counts.at(-1), static_cast<std::size_t>(232));
    CHECK_EQ(demo.accelerate_decelerate_counts.at(0), static_cast<std::size_t>(5519));
    CHECK_EQ(demo.accelerate_decelerate_counts.at(1), static_cast<std::size_t>(647));
    CHECK_EQ(demo.left_right_counts.at(-1), static_cast<std::size_t>(183));
    CHECK_EQ(demo.left_right_counts.at(0), static_cast<std::size_t>(5878));
    CHECK_EQ(demo.left_right_counts.at(1), static_cast<std::size_t>(337));
    CHECK_EQ(demo.jump_counts.false_count, static_cast<std::size_t>(5558));
    CHECK_EQ(demo.jump_counts.true_count, static_cast<std::size_t>(840));
}

static void test_roads() {
    RoadDescriptor d = analyze_road_descriptor(0x4A37);
    CHECK_EQ(d.low_byte, static_cast<uint8_t>(0x37));
    CHECK_EQ(d.high_byte, static_cast<uint8_t>(0x4A));
    CHECK_EQ(d.dispatch_kind, static_cast<uint8_t>(0x0A));
    CHECK_EQ(d.dispatch_variant_low3, static_cast<uint8_t>(0x02));
    CHECK_EQ(d.high_flags, static_cast<uint8_t>(0x04));

    RoadsArchive roads = load_roads_lzs_path(check::asset("ROADS.LZS"));
    CHECK_EQ(roads.road_count(), static_cast<std::size_t>(31));
    CHECK_EQ(roads.used_dispatch_kinds(),
             (std::vector<uint8_t>{0, 1, 2, 3, 4, 5}));
    CHECK_EQ(roads.distinct_descriptor_count(), static_cast<std::size_t>(170));

    std::vector<std::pair<uint8_t, std::size_t>> counts;
    for (const auto& e : roads.descriptor_catalog.dispatch_kinds) {
        counts.emplace_back(e.dispatch_kind, e.count);
    }
    const std::vector<std::pair<uint8_t, std::size_t>> expected = {
        {0, 25781}, {1, 987}, {2, 2132}, {3, 268}, {4, 1079}, {5, 189}};
    CHECK_EQ(counts, expected);
}

static void test_level() {
    RoadsArchive roads = load_roads_lzs_path(check::asset("ROADS.LZS"));
    std::vector<Level> levels = levels_from_roads_archive(roads);
    CHECK_EQ(levels.size(), static_cast<std::size_t>(31));
    CHECK_EQ(levels[0].name, std::string("Demo Level"));
    CHECK_EQ(levels[0].gravity, static_cast<uint16_t>(8));
    CHECK_EQ(levels[0].fuel, static_cast<uint16_t>(130));
    CHECK_EQ(levels[0].oxygen, static_cast<uint16_t>(60));
    CHECK_EQ(levels[0].length(), static_cast<std::size_t>(160));

    Level road0 = level_from_road_entry(roads.roads[0]);
    LevelCell cell_a = road0.cells[83][0];
    CHECK_EQ(cell_a.raw_descriptor, static_cast<uint16_t>(0x0400));
    CHECK_TRUE(cell_a.cube_height == std::optional<uint16_t>(120));
    CHECK_TRUE(!cell_a.has_tile);
    CHECK_TRUE(!cell_a.has_tunnel);

    Level road2 = level_from_road_entry(roads.roads[2]);
    LevelCell cell_b = road2.cells[80][0];
    CHECK_EQ(cell_b.raw_descriptor, static_cast<uint16_t>(0x0507));
    CHECK_TRUE(cell_b.cube_height == std::optional<uint16_t>(120));
    CHECK_TRUE(cell_b.has_tile);
    CHECK_TRUE(cell_b.has_tunnel);
    CHECK_TRUE(cell_b.tile_effect == TouchEffect::None);

    CHECK_EQ(road0.get_cell(95.0, GROUND_Y, 83.2).raw_descriptor,
             static_cast<uint16_t>(0x0400));
    CHECK_EQ(road0.get_cell(95.0 + 46.0 * 6.0, GROUND_Y, 83.2).raw_descriptor,
             static_cast<uint16_t>(0x0400));
}

static void test_trekdat() {
    TrekdatArchive trekdat = load_trekdat_lzs_path(check::asset("TREKDAT.LZS"));
    CHECK_EQ(trekdat.record_count(), static_cast<std::size_t>(8));

    std::vector<uint16_t> expanded_sizes;
    for (const auto& r : trekdat.records) expanded_sizes.push_back(r.load_buff_end);
    CHECK_EQ(expanded_sizes, (std::vector<uint16_t>{24716, 25775, 26324, 26702,
                                                    27278, 26780, 26399, 26153}));

    std::vector<std::size_t> compressed_sizes;
    for (const auto& r : trekdat.records) compressed_sizes.push_back(r.compressed_size);
    CHECK_EQ(compressed_sizes, (std::vector<std::size_t>{11368, 12190, 12397, 12376,
                                                         12592, 12502, 12403, 12320}));

    std::vector<std::size_t> unique_pointer_counts;
    for (const auto& r : trekdat.records)
        unique_pointer_counts.push_back(r.unique_pointer_count());
    CHECK_EQ(unique_pointer_counts,
             (std::vector<std::size_t>{312, 312, 312, 312, 312, 312, 312, 312}));

    std::vector<std::size_t> total_span_counts;
    for (const auto& r : trekdat.records)
        total_span_counts.push_back(r.total_span_count());
    CHECK_EQ(total_span_counts, (std::vector<std::size_t>{1788, 1855, 1886, 1935,
                                                          1989, 1985, 1963, 1910}));

    TrekdatDosPointerLayout layout = trekdat.records[0].dos_pointer_layout();
    CHECK_EQ(layout.rows[0].cells[0].pointers,
             (std::array<uint16_t, 6>{624, 636, 640, 648, 660, 692}));
    CHECK_EQ(layout.rows[11].cells[0].pointers,
             (std::array<uint16_t, 6>{19939, 20146, 20201, 20377, 20560, 20997}));
    CHECK_EQ(layout.rows[12].cells[3].pointers,
             (std::array<uint16_t, 6>{24636, 24648, 24652, 24660, 24672, 24704}));

    auto first_shape = trekdat.records[0].shape_at_offset(944);
    CHECK_TRUE(first_shape.has_value());
    CHECK_EQ(first_shape->color, static_cast<uint8_t>(1));
    CHECK_EQ(first_shape->span_count, static_cast<std::size_t>(3));
    auto next_off = trekdat.records[0].next_shape_offset(944);
    CHECK_TRUE(next_off.has_value());
    auto next_shape = trekdat.records[0].shape_at_offset(*next_off);
    CHECK_TRUE(next_shape.has_value());
    CHECK_EQ(next_shape->color, static_cast<uint8_t>(31));
}

static void test_muzax() {
    MuzaxArchive muzax = load_muzax_lzs_path(check::asset("MUZAX.LZS"));
    CHECK_EQ(muzax.song_table_size, static_cast<uint16_t>(120));
    CHECK_EQ(muzax.song_count(), static_cast<std::size_t>(20));
    CHECK_EQ(muzax.populated_song_count(), static_cast<std::size_t>(14));

    const MuzaxSong& song0 = muzax.songs[0];
    const std::optional<std::array<uint8_t, 3>> expected_widths =
        std::array<uint8_t, 3>{6, 10, 12};
    CHECK_TRUE(song0.widths == expected_widths);
    CHECK_TRUE(song0.compressed_size == std::optional<std::size_t>(1302));
    CHECK_EQ(song0.instrument_bytes, static_cast<std::size_t>(144));
    CHECK_EQ(song0.command_bytes, static_cast<std::size_t>(12174));
    CHECK_TRUE(song0.command_summary.has_value());
    CHECK_EQ(song0.command_summary->function_counts,
             (std::array<std::size_t, 8>{1444, 9, 3718, 909, 4, 1, 1, 1}));
}

static void test_exe() {
    SkyroadsExe exe = load_skyroads_exe_path(check::asset("SKYROADS.EXE"));
    CHECK_EQ(exe.header_bytes, static_cast<std::size_t>(512));
    CHECK_EQ(exe.image_size, static_cast<std::size_t>(29960));
    CHECK_EQ(exe.entry_file_offset, static_cast<std::size_t>(25296));
    CHECK_EQ(exe.exe_reader_base_file_offset, static_cast<std::size_t>(26848));
    CHECK_EQ(exe.relocations.size(), static_cast<std::size_t>(2));
    CHECK_EQ(exe.relocations[0].file_offset, static_cast<std::size_t>(15534));
    CHECK_EQ(exe.relocations[1].file_offset, static_cast<std::size_t>(25297));
    CHECK_EQ(exe.runtime_tables.tile_class_by_low3.values,
             (Bytes{1, 2, 3, 3, 4, 4, 1, 1}));
    std::vector<uint16_t> dispatch_targets;
    for (const auto& e : exe.runtime_tables.draw_dispatch_by_type.entries)
        dispatch_targets.push_back(e.target);
    CHECK_EQ(dispatch_targets,
             (std::vector<uint16_t>{0x2E50, 0x303D, 0x2E9F, 0x2EE1, 0x2F3C, 0x2FB0,
                                    0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD,
                                    0x3AAD, 0x3AAD, 0x3AAD, 0x3AAD}));
}


// skyroads.cfg: 66 bytes, word 0 a checksum, words 3.. the 30 completion counts.
// A missing file or a bad checksum resets progress, as the original does.
static void test_game_config_roundtrip() {
    using skyroads::data::GameConfig;
    const std::string path = "test_skyroads_cfg.tmp";
    std::remove(path.c_str());

    GameConfig fresh = skyroads::data::load_game_config(path);
    for (auto count : fresh.road_completions) CHECK_EQ(count, static_cast<uint16_t>(0));

    GameConfig saved;
    saved.setting_a = 0x1234;
    saved.setting_b = 7;
    saved.road_completions[0] = 3;
    saved.road_completions[29] = 1;
    CHECK_TRUE(skyroads::data::save_game_config(path, saved));

    GameConfig loaded = skyroads::data::load_game_config(path);
    CHECK_EQ(loaded.setting_a, static_cast<uint16_t>(0x1234));
    CHECK_EQ(loaded.road_completions[0], static_cast<uint16_t>(3));
    CHECK_EQ(loaded.road_completions[29], static_cast<uint16_t>(1));

    // Corrupt the checksum word -> everything resets.
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    CHECK_TRUE(f != nullptr);
    const unsigned char bad[2] = {0xAA, 0xBB};
    std::fwrite(bad, 1, 2, f);
    std::fclose(f);
    GameConfig reset = skyroads::data::load_game_config(path);
    for (auto count : reset.road_completions) CHECK_EQ(count, static_cast<uint16_t>(0));
    std::remove(path.c_str());
}

CHECK_MAIN_BEGIN()
    test_game_config_roundtrip();
    test_compression();
    test_image();
    test_sound();
    test_dashboard();
    test_demo();
    test_roads();
    test_level();
    test_trekdat();
    test_muzax();
    test_exe();
CHECK_MAIN_END()
