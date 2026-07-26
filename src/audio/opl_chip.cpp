// Part of the SkyRoads SDL port
#include "audio/opl_chip.hpp"

#include "ymfm_opl.h"

#include <algorithm>
#include <deque>
#include <utility>

namespace skyroads::audio {
namespace {

// ymfm wants an interface object for timers/IRQs; an OPL2 driven by register
// writes alone needs none of that, so the defaults are fine.
class NullYmfmInterface : public ymfm::ymfm_interface {};

} // namespace

struct OplChip::Impl {
    NullYmfmInterface interface;
    ymfm::ym3812 chip;
    uint32_t output_rate;
    uint32_t chip_rate;
    // Resampling state: `phase` walks from one chip sample to the next in units of
    // output samples, so we linearly interpolate between `prev` and `next`.
    double phase = 0.0;
    float prev = 0.0f;
    float next = 0.0f;
    // Register writes waiting for their settling delay; one is applied per chip
    // sample, so consecutive writes land about 20 us apart as they do on hardware.
    std::deque<std::pair<uint8_t, uint8_t>> pending;

    explicit Impl(uint32_t rate)
        : chip(interface), output_rate(rate == 0 ? 48000 : rate) {
        chip_rate = chip.sample_rate(OPL2_CLOCK);
        if (chip_rate == 0) chip_rate = 49716;
    }

    float generate_chip_sample() {
        if (!pending.empty()) {
            const auto write = pending.front();
            pending.pop_front();
            chip.write_address(write.first);
            chip.write_data(write.second);
        }
        ymfm::ym3812::output_data out;
        chip.generate(&out, 1);
        // ymfm returns roughly 16-bit-ish signed data for one OPL2 output channel.
        return static_cast<float>(out.data[0]) / 32768.0f;
    }
};

OplChip::OplChip(uint32_t output_rate)
    : impl_(std::make_unique<Impl>(output_rate)) {
    reset();
}

OplChip::~OplChip() = default;
OplChip::OplChip(OplChip&&) noexcept = default;
OplChip& OplChip::operator=(OplChip&&) noexcept = default;

void OplChip::reset() {
    impl_->chip.reset();
    impl_->pending.clear();
    impl_->phase = 0.0;
    impl_->prev = 0.0f;
    impl_->next = impl_->generate_chip_sample();
}

void OplChip::write(uint8_t reg, uint8_t value) {
    impl_->pending.emplace_back(reg, value);
}

float OplChip::next_sample() {
    const double step =
        static_cast<double>(impl_->chip_rate) / static_cast<double>(impl_->output_rate);
    impl_->phase += step;
    while (impl_->phase >= 1.0) {
        impl_->phase -= 1.0;
        impl_->prev = impl_->next;
        impl_->next = impl_->generate_chip_sample();
    }
    const float t = static_cast<float>(impl_->phase);
    return impl_->prev + (impl_->next - impl_->prev) * t;
}

} // namespace skyroads::audio
