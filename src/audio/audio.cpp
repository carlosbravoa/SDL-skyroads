#include "audio/audio.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "data/sound.hpp"

namespace skyroads::audio {
namespace {

constexpr float MUSIC_TICK_SECONDS = 0.005f;
constexpr float INTRO_GAIN = 0.40f;
constexpr float MUSIC_GAIN = 0.32f;
constexpr float MIN_DB = -96.0f;
constexpr float MAX_DB = 0.0f;
constexpr float PI = 3.14159265358979323846f;

const float NAN_F = std::numeric_limits<float>::quiet_NaN();

const std::array<float, 64> ATTACK_RATES = {
    NAN_F,  NAN_F,  NAN_F,  NAN_F,  2826.24f, 2252.80f, 1884.16f, 1597.44f,
    1413.12f, 1126.40f, 942.08f, 798.72f, 706.56f, 563.20f, 471.04f, 399.36f,
    353.28f, 281.60f, 235.52f, 199.68f, 176.76f, 140.80f, 117.76f, 99.84f,
    88.32f, 70.40f, 58.88f, 49.92f, 44.16f, 35.20f, 29.44f, 24.96f,
    22.08f, 17.60f, 14.72f, 12.48f, 11.04f, 8.80f, 7.36f, 6.24f,
    5.52f, 4.40f, 3.68f, 3.12f, 2.76f, 2.20f, 1.84f, 1.56f,
    1.40f, 1.12f, 0.92f, 0.80f, 0.70f, 0.56f, 0.46f, 0.42f,
    0.38f, 0.30f, 0.24f, 0.20f, 0.0f, 0.0f, 0.0f, 0.0f};

const std::array<float, 64> DECAY_RATES = {
    NAN_F,   NAN_F,   NAN_F,   NAN_F,   39280.64f, 31416.32f, 26173.44f, 22446.08f,
    19640.32f, 15708.16f, 13086.72f, 11223.04f, 9820.16f, 7854.08f, 6543.36f, 5611.52f,
    4910.08f, 3927.04f, 3271.68f, 2805.76f, 2455.04f, 1936.52f, 1635.84f, 1402.88f,
    1227.52f, 981.76f, 817.92f, 701.44f, 613.76f, 490.88f, 488.96f, 350.72f,
    306.88f, 245.44f, 204.48f, 175.36f, 153.44f, 122.72f, 102.24f, 87.68f,
    76.72f, 61.36f, 51.12f, 43.84f, 38.36f, 30.68f, 25.56f, 21.92f,
    19.20f, 15.36f, 12.80f, 10.96f, 9.60f, 7.68f, 6.40f, 5.48f,
    4.80f, 3.84f, 3.20f, 2.74f, 2.40f, 2.40f, 2.40f, 2.40f};

const std::array<float, 4> KEY_SCALE_MULTIPLIERS = {0.0f, 1.0f, 0.5f, 2.0f};
const std::array<float, 8> FREQ_STARTS = {0.047f, 0.094f, 0.189f, 0.379f,
                                          0.758f, 1.517f, 3.034f, 6.068f};
const std::array<float, 8> FREQ_STEPS = {0.048f, 0.095f, 0.190f, 0.379f,
                                         0.759f, 1.517f, 3.034f, 6.069f};
const std::array<std::array<float, 16>, 8> KEY_SCALE_LEVELS = {{
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0.75f, 1.125f, 1.5f, 1.875f, 2.25f, 2.625f, 3.0f},
    {0, 0, 0, 0, 0, 1.875f, 3.0f, 4.125f, 4.875f, 5.625f, 6.0f, 6.75f, 7.125f, 7.5f, 7.875f, 8.25f},
    {0, 0, 0, 1.875f, 3.0f, 4.125f, 4.875f, 5.625f, 6.0f, 6.75f, 7.125f, 7.5f, 7.875f, 8.25f, 8.625f, 9.0f},
    {0, 0, 3.0f, 4.875f, 6.0f, 7.125f, 7.875f, 8.625f, 9.0f, 9.75f, 10.125f, 10.5f, 10.875f, 11.25f, 11.625f, 12.0f},
    {0, 3.0f, 6.0f, 7.875f, 9.0f, 10.125f, 10.875f, 11.625f, 12.0f, 12.75f, 13.125f, 13.5f, 13.875f, 14.25f, 14.625f, 15.0f},
    {0, 6.0f, 9.0f, 10.875f, 12.0f, 13.125f, 13.875f, 14.625f, 15.0f, 15.75f, 16.125f, 16.5f, 16.875f, 17.25f, 17.625f, 18.0f},
    {0, 9.0f, 12.0f, 13.875f, 15.0f, 16.125f, 16.875f, 17.625f, 18.0f, 18.75f, 19.125f, 19.5f, 19.875f, 20.25f, 20.625f, 21.0f},
}};

std::size_t steps_from_ms(float time_ms, float sample_rate) {
    const float inner = time_ms / 1000.0f * (1.0f / sample_rate);
    const float steps = std::max(std::floor(1.0f / inner), 1.0f);
    return static_cast<std::size_t>(steps);
}

std::size_t sat_sub(std::size_t a, std::size_t b) { return a > b ? a - b : 0; }

OscDesc osc_desc_from_instrument(const MuzaxOscillator& osc, WaveType wave_form) {
    OscDesc d;
    d.tremolo = osc.tremolo;
    d.vibrato = osc.vibrato;
    d.sound_sustaining = osc.sound_sustaining;
    d.key_scaling = osc.key_scaling;
    d.multiplication = osc.multiplication == 0 ? 0.0f : static_cast<float>(osc.multiplication);
    d.key_scale_level = osc.key_scale_level;
    d.output_level = (static_cast<float>(osc.output_level) / 0x3F) * -47.25f;
    d.attack_rate = osc.attack_rate;
    d.decay_rate = osc.decay_rate;
    d.sustain_level = -45.0f * static_cast<float>(osc.sustain_level) / 0x0F;
    d.release_rate = osc.release_rate;
    d.wave_form = wave_form;
    return d;
}

void configure_osc_start(OscState& osc, uint16_t current_freq_num,
                         uint8_t current_block_num, uint16_t freq_num,
                         uint8_t block_num) {
    if (current_freq_num == freq_num && current_block_num == block_num &&
        osc.state == KeyState::Sustain) {
        return;
    }
    osc.state = KeyState::Attack;
    osc.envelope_step = 0;
}

float process_osc(float sample_rate, float time,
                  const std::array<std::array<float, SAMPLE_COUNT_WAVE>, 8>& waves,
                  OscState& osc, uint16_t freq_num, uint8_t block_num,
                  float modulator) {
    if (osc.state == KeyState::Off) return 0.0f;

    const std::size_t key_scale_num =
        static_cast<std::size_t>(block_num) * 2 + static_cast<std::size_t>(freq_num >> 7);
    const std::size_t rof = osc.config.key_scaling ? key_scale_num : key_scale_num / 4;
    auto get_rate = [&](std::size_t rate) -> std::size_t {
        return rate > 0 ? std::min<std::size_t>(rof + rate * 4, 63) : 0;
    };

    switch (osc.state) {
        case KeyState::Attack: {
            const std::size_t rate = get_rate(osc.config.attack_rate);
            const float time_to_attack = ATTACK_RATES[rate];
            if (time_to_attack == 0.0f) {
                osc.volume = MAX_DB;
                osc.envelope_step = 0;
                osc.state = KeyState::Decay;
            } else if (std::isnan(time_to_attack)) {
                osc.state = KeyState::Off;
            } else {
                const std::size_t steps = steps_from_ms(time_to_attack, sample_rate);
                const float p = 3.0f;
                osc.volume = -96.0f *
                             std::pow(static_cast<float>(sat_sub(steps, osc.envelope_step)) /
                                          static_cast<float>(steps),
                                      p);
                osc.envelope_step += 1;
                if (osc.envelope_step >= steps) {
                    osc.envelope_step = 0;
                    osc.volume = MAX_DB;
                    osc.state = KeyState::Decay;
                }
            }
            break;
        }
        case KeyState::Decay: {
            const std::size_t rate = get_rate(osc.config.decay_rate);
            const float time_to_decay = DECAY_RATES[rate];
            if (time_to_decay == 0.0f) {
                osc.volume = osc.config.sustain_level;
                osc.envelope_step = 0;
                osc.state = KeyState::Sustain;
            } else if (!std::isnan(time_to_decay)) {
                const std::size_t steps = steps_from_ms(time_to_decay, sample_rate);
                const float decrease_amt = osc.config.sustain_level / static_cast<float>(steps);
                osc.volume += decrease_amt;
                osc.envelope_step += 1;
                if (osc.envelope_step >= steps) {
                    osc.envelope_step = 0;
                    osc.state = KeyState::Sustain;
                }
            }
            break;
        }
        case KeyState::Sustain:
            if (!osc.config.sound_sustaining) osc.state = KeyState::Release;
            break;
        case KeyState::Release: {
            const std::size_t rate = get_rate(osc.config.release_rate);
            const float time_to_release = DECAY_RATES[rate];
            const std::size_t steps = steps_from_ms(time_to_release, sample_rate);
            const float decrease_amt =
                (MIN_DB - osc.config.sustain_level) / static_cast<float>(steps);
            osc.volume += decrease_amt;
            osc.envelope_step += 1;
            if (osc.envelope_step >= steps) {
                osc.volume = MIN_DB;
                osc.state = KeyState::Off;
            }
            break;
        }
        case KeyState::Off:
            break;
    }

    float ks_damping = 0.0f;
    if (osc.config.key_scale_level > 0) {
        const float kslm = KEY_SCALE_MULTIPLIERS[osc.config.key_scale_level];
        ks_damping = -kslm * KEY_SCALE_LEVELS[block_num][freq_num >> 6];
    }

    float freq = FREQ_STARTS[block_num] +
                 FREQ_STEPS[block_num] * static_cast<float>(freq_num);
    freq *= osc.config.multiplication == 0.0f ? 0.5f : osc.config.multiplication;

    const float vib = osc.config.vibrato
                          ? std::cos(time * 2.0f * PI) * 0.00004f + 1.0f
                          : 1.0f;
    osc.angle += (1.0f / sample_rate) * 2.0f * PI * freq * vib;

    const float angle = osc.angle + modulator;
    const float wrapped = std::fmod(std::fabs(angle), 2.0f * PI);
    const std::size_t wave_index = static_cast<std::size_t>(std::min(
        std::floor((wrapped * static_cast<float>(SAMPLE_COUNT_WAVE)) / (2.0f * PI)),
        static_cast<float>(SAMPLE_COUNT_WAVE - 1)));
    const float wave = waves[static_cast<std::size_t>(osc.config.wave_form)][wave_index];
    const float tremolo = osc.config.tremolo
                              ? std::fabs(std::cos(time * PI * 3.7f))
                              : 0.0f;
    return wave *
           std::pow(10.0f,
                    (osc.volume + osc.config.output_level + tremolo + ks_damping) / 10.0f);
}

} // namespace

WaveType wave_type_from_u8(uint8_t value) {
    switch (value & 7) {
        case 0: return WaveType::Sine;
        case 1: return WaveType::HalfSine;
        case 2: return WaveType::AbsSign;
        case 3: return WaveType::PulseSign;
        case 4: return WaveType::SineEven;
        case 5: return WaveType::AbsSineEven;
        case 6: return WaveType::Square;
        default: return WaveType::DerivedSquare;
    }
}

// ---- OplSynth --------------------------------------------------------------

OplSynth::OplSynth(float sample_rate)
    : sample_rate_(sample_rate), time_(0.0f), channels_(15) {
    for (auto& row : waves_) row.fill(0.0f);
    for (std::size_t index = 0; index < SAMPLE_COUNT_WAVE; ++index) {
        const float angle =
            2.0f * PI * static_cast<float>(index) / static_cast<float>(SAMPLE_COUNT_WAVE);
        const float sine = std::sin(angle);
        waves_[static_cast<std::size_t>(WaveType::Sine)][index] = sine;
        waves_[static_cast<std::size_t>(WaveType::HalfSine)][index] = std::max(sine, 0.0f);
        waves_[static_cast<std::size_t>(WaveType::AbsSign)][index] = std::fabs(sine);
        waves_[static_cast<std::size_t>(WaveType::PulseSign)][index] =
            std::fmod(angle, 6.28f) < 1.57f ? sine : 0.0f;
        waves_[static_cast<std::size_t>(WaveType::SineEven)][index] =
            std::fmod(angle, 12.56f) < 6.28f ? sine : 0.0f;
        waves_[static_cast<std::size_t>(WaveType::AbsSineEven)][index] =
            std::fmod(angle, 12.56f) < 6.28f ? std::fabs(sine) : 0.0f;
        waves_[static_cast<std::size_t>(WaveType::Square)][index] = sine > 0.0f ? 1.0f : 0.0f;
        waves_[static_cast<std::size_t>(WaveType::DerivedSquare)][index] =
            sine > 0.0f ? 1.0f : 0.0f;
    }
}

void OplSynth::stop_all() {
    for (auto& channel : channels_) {
        channel.a.state = KeyState::Off;
        channel.b.state = KeyState::Off;
    }
}

void OplSynth::set_channel_config(std::size_t channel_index,
                                  const MuzaxInstrument& instrument) {
    if (channel_index >= channels_.size()) return;
    Channel& channel = channels_[channel_index];
    channel.a.config = osc_desc_from_instrument(
        instrument.operator_a, wave_type_from_u8(instrument.operator_a.wave_form));
    channel.b.config = osc_desc_from_instrument(
        instrument.operator_b, wave_type_from_u8(instrument.operator_b.wave_form));
    channel.additive = (instrument.channel_config & 1) != 0;
    channel.feedback = static_cast<std::size_t>((instrument.channel_config & 0x0E) >> 1);
    channel.feedback_factor =
        channel.feedback > 0 ? std::pow(2.0f, static_cast<float>(channel.feedback) + 8.0f)
                             : 0.0f;
    const float radians_per_wave = 2.0f * PI;
    const float dbu_per_wave = 1024.0f * 16.0f;
    const float vol_as_dbu = 1.0f * 0x4000 * 0x10000 / 0x4000;
    channel.m2 = radians_per_wave * vol_as_dbu / dbu_per_wave;
    channel.m1 = channel.m2 / 2.0f / 0x10000;
}

void OplSynth::set_channel_volume(std::size_t channel_index, float volume) {
    if (channel_index >= channels_.size()) return;
    channels_[channel_index].b.config.output_level = volume;
}

void OplSynth::start_note(std::size_t channel_index, uint16_t freq_num,
                          uint8_t block_num) {
    if (channel_index >= channels_.size()) return;
    Channel& channel = channels_[channel_index];
    configure_osc_start(channel.a, channel.freq_num, channel.block_num, freq_num, block_num);
    configure_osc_start(channel.b, channel.freq_num, channel.block_num, freq_num, block_num);
    channel.freq_num = freq_num;
    channel.block_num = block_num;
}

void OplSynth::stop_note(std::size_t channel_index) {
    if (channel_index >= channels_.size()) return;
    Channel& channel = channels_[channel_index];
    for (OscState* osc : {&channel.a, &channel.b}) {
        if (osc->state != KeyState::Off) osc->state = KeyState::Release;
    }
}

float OplSynth::next_sample() {
    time_ += 1.0f / sample_rate_;
    float out = 0.0f;
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        out += process_channel(i);
    }
    return std::clamp(out / 2.0f, -1.0f, 1.0f);
}

float OplSynth::process_channel(std::size_t channel_index) {
    Channel& channel = channels_[channel_index];
    const float feedback_mod =
        (channel.output_0 + channel.output_1) * channel.feedback_factor * channel.m1;
    const float a = process_osc(sample_rate_, time_, waves_, channel.a,
                                channel.freq_num, channel.block_num, feedback_mod);
    const float b = process_osc(sample_rate_, time_, waves_, channel.b,
                                channel.freq_num, channel.block_num,
                                channel.additive ? 0.0f : a * channel.m2);
    channel.output_1 = channel.output_0;
    channel.output_0 = a;
    return channel.additive ? a + b : b;
}

// ---- ActivePcm -------------------------------------------------------------

ActivePcm::ActivePcm(Pcm8Sample sample_in, float gain_in)
    : sample(std::move(sample_in)), position(0.0), gain(gain_in), finished(false) {}

float ActivePcm::next_sample(uint32_t output_rate) {
    if (finished) return 0.0f;
    const std::size_t index = static_cast<std::size_t>(std::floor(position));
    if (index >= sample.samples.size()) {
        finished = true;
        return 0.0f;
    }
    const uint8_t byte = sample.samples[index];
    position += static_cast<double>(sample.sample_rate) / static_cast<double>(output_rate);
    return ((static_cast<float>(byte) / 255.0f) * 2.0f - 1.0f) * gain;
}

// ---- MuzaxPlayer -----------------------------------------------------------

MuzaxPlayer::MuzaxPlayer(MuzaxArchive muzax) : muzax_(std::move(muzax)) {}

void MuzaxPlayer::load_song(std::size_t song_index, OplSynth& synth) {
    if (current_song_ == std::optional<std::size_t>(song_index)) return;
    current_song_ = song_index;
    cursor_ = 0;
    paused_ = 0;
    jump_pos_ = 0;
    time_until_tick_ = 0.0f;
    commands_.clear();
    if (song_index < muzax_.songs.size() && muzax_.songs[song_index].commands) {
        commands_ = *muzax_.songs[song_index].commands;
    }
    synth.stop_all();
}

void MuzaxPlayer::stop(OplSynth& synth) {
    current_song_.reset();
    commands_.clear();
    cursor_ = 0;
    paused_ = 0;
    jump_pos_ = 0;
    time_until_tick_ = 0.0f;
    synth.stop_all();
}

void MuzaxPlayer::render(OplSynth& synth, std::vector<float>& out) {
    if (!current_song_) {
        std::fill(out.begin(), out.end(), 0.0f);
        return;
    }
    const float dt = 1.0f / static_cast<float>(OUTPUT_SAMPLE_RATE);
    for (float& sample : out) {
        time_until_tick_ += dt;
        while (time_until_tick_ >= MUSIC_TICK_SECONDS) {
            read_note(synth);
            time_until_tick_ -= MUSIC_TICK_SECONDS;
        }
        sample = synth.next_sample();
    }
}

void MuzaxPlayer::read_note(OplSynth& synth) {
    if (!current_song_ || commands_.empty()) return;
    if (paused_ > 0) {
        paused_ -= 1;
        return;
    }

    while (paused_ == 0) {
        if (cursor_ + 1 >= commands_.size()) cursor_ = 0;

        uint8_t cmd_low = commands_[cursor_];
        const uint8_t cmd_high = commands_[cursor_ + 1];
        cursor_ += 2;

        const uint8_t function_type = cmd_low & 7;
        cmd_low >>= 4;

        switch (function_type) {
            case 0:
                paused_ = cmd_high;
                return;
            case 1:
                stop_note(cmd_low, synth);
                configure_instrument(cmd_low, cmd_high, synth);
                break;
            case 2:
                play_note(cmd_low, cmd_high, synth);
                break;
            case 3:
                stop_note(cmd_low, synth);
                break;
            case 4:
                synth.set_channel_volume(
                    cmd_low, static_cast<float>(cmd_high & 0x3F) / 0x3F * -47.25f);
                break;
            case 5:
                cursor_ = std::min(jump_pos_, commands_.size());
                break;
            case 6:
                jump_pos_ = cursor_;
                break;
            default:
                break;
        }
    }
}

void MuzaxPlayer::configure_instrument(std::size_t channel,
                                       std::size_t instrument_index,
                                       OplSynth& synth) {
    if (!current_song_) return;
    const std::size_t song_index = *current_song_;
    if (song_index >= muzax_.songs.size()) return;
    const auto& instruments = muzax_.songs[song_index].instruments;
    if (instrument_index >= instruments.size()) return;
    synth.set_channel_config(channel, instruments[instrument_index]);
}

void MuzaxPlayer::stop_note(std::size_t channel, OplSynth& synth) {
    if (channel < 11) synth.stop_note(channel);
}

void MuzaxPlayer::play_note(std::size_t channel, uint8_t note, OplSynth& synth) {
    static const int low_freqs[12] = {0xAC, 0xB6, 0xC1, 0xCD, 0xD9, 0xE6,
                                      0xF3, 0x02, 0x11, 0x22, 0x33, 0x45};
    static const int high_freqs[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    const std::size_t note_idx = note % 12;
    const std::size_t octave = static_cast<std::size_t>(note / 12) + 2;
    const uint16_t freq_num = static_cast<uint16_t>(
        (static_cast<uint16_t>(high_freqs[note_idx]) << 8) | low_freqs[note_idx]);
    const std::size_t target = channel < 6 ? channel : channel - 6 + 6;
    synth.start_note(target, freq_num, static_cast<uint8_t>(octave));
}

// ---- AttractAudioAssets ----------------------------------------------------

AttractAudioAssets AttractAudioAssets::load_from_root(const std::string& source_root) {
    auto path = [&](const std::string& name) {
        if (!source_root.empty() && source_root.back() == '/') return source_root + name;
        return source_root + "/" + name;
    };
    AttractAudioAssets a;
    a.intro = skyroads::data::load_intro_snd_path(path("INTRO.SND"));
    a.sfx = skyroads::data::load_sfx_snd_path(path("SFX.SND"));
    a.muzax = skyroads::data::load_muzax_lzs_path(path("MUZAX.LZS"));
    return a;
}

// ---- AudioMixer ------------------------------------------------------------

AudioMixer::AudioMixer(AttractAudioAssets assets)
    : assets_(std::move(assets)),
      synth_(static_cast<float>(OUTPUT_SAMPLE_RATE)),
      player_(assets_.muzax) {}

void AudioMixer::apply_commands(
    const std::vector<skyroads::core::AudioCommand>& commands) {
    using skyroads::core::AudioCommandKind;
    for (const auto& command : commands) {
        switch (command.kind) {
            case AudioCommandKind::PlaySong:
                player_.load_song(command.value, synth_);
                timeline_.push_back({AudioTimelineKind::PlaySong, command.value});
                break;
            case AudioCommandKind::StopSong:
                player_.stop(synth_);
                timeline_.push_back({AudioTimelineKind::StopSong, 0});
                break;
            case AudioCommandKind::PlayIntroSample:
                active_samples_.emplace_back(assets_.intro, INTRO_GAIN);
                timeline_.push_back({AudioTimelineKind::PlayIntroSample, 0});
                break;
            case AudioCommandKind::PlaySfx:
                if (command.value < assets_.sfx.effects.size()) {
                    active_samples_.emplace_back(
                        assets_.sfx.effects[command.value].sample, 0.55f);
                    timeline_.push_back({AudioTimelineKind::PlaySfx, command.value});
                }
                break;
            case AudioCommandKind::StopAllSamples:
                active_samples_.clear();
                timeline_.push_back({AudioTimelineKind::StopAllSamples, 0});
                break;
        }
    }
}

std::vector<int16_t> AudioMixer::render_i16(std::size_t sample_count) {
    std::vector<int16_t> out(sample_count, 0);
    render_into(out);
    return out;
}

void AudioMixer::render_into(std::vector<int16_t>& out) {
    std::vector<float> music_accum(out.size(), 0.0f);
    player_.render(synth_, music_accum);

    for (std::size_t index = 0; index < out.size(); ++index) {
        float mixed = music_accum[index] * MUSIC_GAIN;
        for (auto& playback : active_samples_) {
            mixed += playback.next_sample(OUTPUT_SAMPLE_RATE);
        }
        out[index] = static_cast<int16_t>(std::clamp(mixed, -1.0f, 1.0f) * 32767.0f);
    }

    active_samples_.erase(
        std::remove_if(active_samples_.begin(), active_samples_.end(),
                       [](const ActivePcm& p) { return p.finished; }),
        active_samples_.end());
}

} // namespace skyroads::audio
