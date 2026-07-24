// Part of the SkyRoads SDL port
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "data/byteio.hpp"

namespace skyroads::data {

// 0x10000 / 0x666 indexing factor: one demo byte per fixed-point tile step.
constexpr uint32_t DEMO_TILE_POSITION_STEP_FP16 = 0x0666;

struct DemoInput {
    std::size_t index = 0;
    uint8_t byte = 0;
    int8_t accelerate_decelerate = 0;
    int8_t left_right = 0;
    bool jump = false;
    uint32_t tile_position_fp16 = 0;
};

struct JumpCounts {
    std::size_t false_count = 0;
    std::size_t true_count = 0;
};

struct DemoRecording {
    std::size_t source_len = 0;
    Bytes raw;
    std::vector<DemoInput> entries;
    std::map<int8_t, std::size_t> accelerate_decelerate_counts;
    std::map<int8_t, std::size_t> left_right_counts;
    JumpCounts jump_counts;

    std::size_t byte_count() const { return raw.size(); }
    uint32_t approx_tile_length_fp16() const {
        return static_cast<uint32_t>(raw.size()) * DEMO_TILE_POSITION_STEP_FP16;
    }
    double approx_tile_length() const {
        return static_cast<double>(approx_tile_length_fp16()) / 65536.0;
    }
};

DemoRecording load_demo_rec_bytes(const Bytes& data);
DemoRecording load_demo_rec_path(const std::string& path);

} // namespace skyroads::data
