// Part of the SkyRoads SDL port
//
// Reference audio path: PCM sample playback (INTRO/SFX) plus a software
// OPL-style synth driving MUZAX songs, mixed to 48 kHz i16. Deterministic and
// device-free so the scheduled timeline is testable, mirroring the reference module.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/app.hpp"
#include "audio/opl_chip.hpp"
#include "data/muzax.hpp"
#include "data/sound.hpp"

namespace skyroads::audio {

using skyroads::data::Bytes;
using skyroads::data::MuzaxArchive;
using skyroads::data::MuzaxInstrument;
using skyroads::data::MuzaxOscillator;
using skyroads::data::Pcm8Sample;
using skyroads::data::SfxBank;

constexpr uint32_t OUTPUT_SAMPLE_RATE = 48000;
constexpr std::size_t SAMPLE_COUNT_WAVE = 1024;

// the reference design payload enum -> tagged struct.
enum class AudioTimelineKind {
    PlaySong,
    StopSong,
    PlayIntroSample,
    PlaySfx,
    StopAllSamples,
};
struct AudioTimelineEvent {
    AudioTimelineKind kind;
    uint8_t value = 0;
    bool operator==(const AudioTimelineEvent& o) const {
        return kind == o.kind && value == o.value;
    }
};

struct AttractAudioAssets {
    Pcm8Sample intro;
    SfxBank sfx;
    MuzaxArchive muzax;
    static AttractAudioAssets load_from_root(const std::string& source_root);
};

enum class WaveType {
    Sine = 0,
    HalfSine = 1,
    AbsSign = 2,
    PulseSign = 3,
    SineEven = 4,
    AbsSineEven = 5,
    Square = 6,
    DerivedSquare = 7,
};
WaveType wave_type_from_u8(uint8_t value);

enum class KeyState { Off, Attack, Sustain, Decay, Release };

struct OscDesc {
    bool tremolo = false;
    bool vibrato = false;
    bool sound_sustaining = true;
    bool key_scaling = false;
    float multiplication = 1.0f;
    std::size_t key_scale_level = 0;
    float output_level = 0.0f;
    std::size_t attack_rate = 0;
    std::size_t decay_rate = 0;
    float sustain_level = 0.0f;
    std::size_t release_rate = 0;
    WaveType wave_form = WaveType::Sine;
};

struct OscState {
    OscDesc config;
    KeyState state = KeyState::Off;
    float volume = -96.0f; // MIN_DB
    std::size_t envelope_step = 0;
    float angle = 0.0f;
};

struct Channel {
    OscState a;
    OscState b;
    bool additive = false;
    std::size_t feedback = 0;
    uint16_t freq_num = 0;
    uint8_t block_num = 0;
    float output_0 = 0.0f;
    float output_1 = 0.0f;
    float feedback_factor = 0.0f;
    float m1 = 0.0f;
    float m2 = 0.0f;
};

// SkyRoads' AdLib driver, reproduced from the EXE (register primitive @0x5876,
// instrument load @0x58fd, note on @0x5955, key off @0x59b3, volume @0x59f1) and
// driving a real emulated YM3812. The game has eleven voices: six melodic plus the
// five OPL rhythm voices, which is why voices 6..10 key on through register 0xBD
// rather than 0xB0.
constexpr std::size_t OPL_VOICES = 11;

class OplSynth {
public:
    explicit OplSynth(float sample_rate);
    void stop_all();
    void set_channel_config(std::size_t channel_index,
                            const MuzaxInstrument& instrument);
    // Raw volume byte from the song, used directly as an attenuation-table index.
    void set_channel_volume(std::size_t channel_index, uint8_t volume);
    void start_note(std::size_t channel_index, uint16_t freq_num, uint8_t block_num);
    void stop_note(std::size_t channel_index);
    float next_sample();

private:
    void init_chip();
    void write_reg(uint8_t reg, uint8_t value);
    void apply_volume(std::size_t channel_index);

    OplChip chip_;
    // Mirror of register 0xBD (AM depth, vibrato depth, rhythm enable and the five
    // rhythm key-on bits): the driver read-modify-writes it, so we must too.
    uint8_t rhythm_reg_ = 0xE0;
    // Instrument bytes last loaded per voice, needed to recompute total level when
    // the volume changes -- the driver re-reads its instrument table for that.
    std::array<std::array<uint8_t, 11>, OPL_VOICES> voice_instrument_{};
    std::array<bool, OPL_VOICES> voice_has_instrument_{};
    std::array<uint8_t, OPL_VOICES> voice_volume_{};
};

struct ActivePcm {
    Pcm8Sample sample;
    double position = 0.0;
    float gain = 0.0f;
    bool finished = false;
    ActivePcm(Pcm8Sample sample, float gain);
    float next_sample(uint32_t output_rate);
};

class MuzaxPlayer {
public:
    explicit MuzaxPlayer(MuzaxArchive muzax);
    void load_song(std::size_t song_index, OplSynth& synth);
    void stop(OplSynth& synth);
    void render(OplSynth& synth, std::vector<float>& out);

private:
    void read_note(OplSynth& synth);
    void configure_instrument(std::size_t channel, std::size_t instrument_index,
                              OplSynth& synth);
    void stop_note(std::size_t channel, OplSynth& synth);
    void play_note(std::size_t channel, uint8_t note, OplSynth& synth);

    MuzaxArchive muzax_;
    std::optional<std::size_t> current_song_;
    Bytes commands_;
    std::size_t cursor_ = 0;
    uint8_t paused_ = 0;
    std::size_t jump_pos_ = 0;
    float time_until_tick_ = 0.0f;
};

class AudioMixer {
public:
    explicit AudioMixer(AttractAudioAssets assets);
    uint32_t output_sample_rate() const { return OUTPUT_SAMPLE_RATE; }
    const std::vector<AudioTimelineEvent>& timeline() const { return timeline_; }
    void apply_commands(const std::vector<skyroads::core::AudioCommand>& commands);
    std::vector<int16_t> render_i16(std::size_t sample_count);
    void render_into(std::vector<int16_t>& out);

private:
    AttractAudioAssets assets_;
    std::vector<AudioTimelineEvent> timeline_;
    std::vector<ActivePcm> active_samples_;
    OplSynth synth_;
    MuzaxPlayer player_;
};

} // namespace skyroads::audio
