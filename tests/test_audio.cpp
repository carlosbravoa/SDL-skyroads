// Equivalence tests for the reference audio path, mirroring the unit-test blocks
// in the reference audio suite: asset counts, the scheduled command timeline, and that
// mixing produces non-silent output.
#include <vector>

#include "audio/audio.hpp"
#include "check.hpp"
#include "core/app.hpp"

using namespace skyroads::audio;
using skyroads::core::AudioCommand;

static void test_assets_load() {
    AttractAudioAssets a = AttractAudioAssets::load_from_root(check::assets_dir());
    CHECK_EQ(a.intro.sample_count(), static_cast<std::size_t>(32100));
    CHECK_EQ(a.sfx.effect_count(), static_cast<std::size_t>(6));
    CHECK_EQ(a.muzax.populated_song_count(), static_cast<std::size_t>(14));
}

static void test_mixer() {
    AttractAudioAssets a = AttractAudioAssets::load_from_root(check::assets_dir());
    AudioMixer mixer(a);
    mixer.apply_commands(
        {AudioCommand::play_song(1), AudioCommand::play_intro_sample()});
    const std::vector<AudioTimelineEvent> expected = {
        {AudioTimelineKind::PlaySong, 1}, {AudioTimelineKind::PlayIntroSample, 0}};
    CHECK_EQ(mixer.timeline(), expected);

    std::vector<int16_t> samples = mixer.render_i16(2048);
    bool any_nonzero = false;
    for (int16_t s : samples) {
        if (s != 0) { any_nonzero = true; break; }
    }
    CHECK_TRUE(any_nonzero);
}

CHECK_MAIN_BEGIN()
    test_assets_load();
    test_mixer();
CHECK_MAIN_END()
