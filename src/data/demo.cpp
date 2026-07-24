#include "data/demo.hpp"

namespace skyroads::data {

DemoRecording load_demo_rec_bytes(const Bytes& data) {
    DemoRecording demo;
    demo.source_len = data.size();
    demo.raw = data;
    demo.entries.reserve(data.size());

    for (std::size_t index = 0; index < data.size(); ++index) {
        const uint8_t value = data[index];
        // Two-bit fields decode to {-1, 0, 1}; jump is a single bit.
        const int8_t accelerate_decelerate =
            static_cast<int8_t>(static_cast<int8_t>(value & 0x03) - 1);
        const int8_t left_right =
            static_cast<int8_t>(static_cast<int8_t>((value >> 2) & 0x03) - 1);
        const bool jump = ((value >> 4) & 0x01) != 0;

        demo.accelerate_decelerate_counts[accelerate_decelerate] += 1;
        demo.left_right_counts[left_right] += 1;
        if (jump) {
            demo.jump_counts.true_count += 1;
        } else {
            demo.jump_counts.false_count += 1;
        }

        DemoInput input;
        input.index = index;
        input.byte = value;
        input.accelerate_decelerate = accelerate_decelerate;
        input.left_right = left_right;
        input.jump = jump;
        input.tile_position_fp16 =
            static_cast<uint32_t>(index) * DEMO_TILE_POSITION_STEP_FP16;
        demo.entries.push_back(input);
    }

    return demo;
}

DemoRecording load_demo_rec_path(const std::string& path) {
    return load_demo_rec_bytes(read_file(path));
}

} // namespace skyroads::data
