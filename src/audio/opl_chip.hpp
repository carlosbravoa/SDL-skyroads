// Part of the SkyRoads SDL port
//
// A real YM3812 (OPL2) — the chip SkyRoads actually drives. The game writes OPL
// registers directly through ports 0x388/0x389 (register-write primitive at image
// 0x5876), so the faithful way to reproduce its music is to feed the very same
// register stream to an emulated chip rather than approximate the FM synthesis.
// Emulation is ymfm (BSD-3-Clause), vendored under third_party/ymfm.
#pragma once

#include <cstdint>
#include <memory>

namespace skyroads::audio {

// The AdLib/Sound Blaster OPL2 master clock.
constexpr uint32_t OPL2_CLOCK = 3579545;

class OplChip {
public:
    explicit OplChip(uint32_t output_rate);
    ~OplChip();
    OplChip(OplChip&&) noexcept;
    OplChip& operator=(OplChip&&) noexcept;

    void reset();
    // One OPL register write, exactly as the game would do to ports 0x388/0x389.
    void write(uint8_t reg, uint8_t value);
    // One sample at the configured output rate, resampled from the chip's native
    // rate (clock/72, about 49716 Hz), normalised to roughly [-1, 1].
    float next_sample();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace skyroads::audio
