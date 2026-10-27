#include <recording.h>
#include "check.h"

using namespace scan;

static std::vector<float> block(int count, float value) {
    return std::vector<float>(count, value);
}

TEST(trimmer_drops_silence_before_the_first_sound) {
    SilenceTrimmer trimmer;
    trimmer.reset(100);
    auto silence = block(64, 0.0f);
    auto sound = block(64, 0.3f);
    CHECK(trimmer.keep(silence.data(), 64) == 0);
    CHECK(trimmer.keep(silence.data(), 64) == 0);
    CHECK(trimmer.keep(sound.data(), 64) == 64);
    CHECK(trimmer.audibleSamples() == 64);
}

TEST(trimmer_shortens_pauses) {
    SilenceTrimmer trimmer;
    trimmer.reset(100);
    auto silence = block(64, 0.0f);
    auto sound = block(64, 0.3f);
    trimmer.keep(sound.data(), 64);
    CHECK(trimmer.keep(silence.data(), 64) == 64);
    CHECK(trimmer.keep(silence.data(), 64) == 36);
    CHECK(trimmer.keep(silence.data(), 64) == 0);

    // The next over gets a full pause again
    CHECK(trimmer.keep(sound.data(), 64) == 64);
    CHECK(trimmer.keep(silence.data(), 64) == 64);
    CHECK(trimmer.audibleSamples() == 128);
}

TEST(trimmer_counts_squelch_fades_as_sound) {
    SilenceTrimmer trimmer;
    trimmer.reset(0);
    auto fade = block(32, 0.001f);
    CHECK(trimmer.keep(fade.data(), 32) == 32);
}

TEST(clip_paths_never_replace_an_earlier_clip) {
    std::set<std::string> existing = { "/r/a.wav", "/r/a_2.wav" };
    auto exists = [&](const std::string& p) { return existing.count(p) > 0; };
    CHECK(uniquePath("/r/a", ".wav", exists) == "/r/a_3.wav");
    CHECK(uniquePath("/r/b", ".wav", exists) == "/r/b.wav");
}

TEST(channels_are_named_as_on_the_radio) {
    // 25kHz channels by their frequency, 8.33kHz ones by their channel name
    CHECK(channelName(118100000.0) == "118.100");
    CHECK(channelName(121500000.0) == "121.500");
    CHECK(channelName(132766666.7) == "132.765");
    CHECK(channelName(130233333.3) == "130.235");
    CHECK(channelName(118008333.3) == "118.010");
    // Off the grid, the frequency rounded
    CHECK(channelName(129474847.0) == "129.475");
}

TEST(bookmark_names_are_safe_in_file_names) {
    CHECK(fileNamePart("Tower") == "Tower");
    CHECK(fileNamePart("LOWK TWR/APP") == "LOWK-TWR-APP");
}

TEST(frequencies_match_within_half_a_step) {
    std::set<long long> freqs = { 121500000 };
    CHECK(matchesAny(freqs, 121500000.0, 25000.0 / 3.0));
    CHECK(matchesAny(freqs, 121503000.0, 25000.0 / 3.0));
    CHECK(!matchesAny(freqs, 121508333.3, 25000.0 / 3.0));
    CHECK(!matchesAny({}, 121500000.0, 25000.0));
}

static void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(v & 0xFF);
    out.push_back(v >> 8);
}

static void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

static void putTag(std::vector<uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

static std::vector<uint8_t> wavFile(uint16_t channels, uint16_t bits, const std::vector<int16_t>& samples, uint32_t dataSize, uint32_t sampleRate = 48000) {
    std::vector<uint8_t> out;
    putTag(out, "RIFF");
    put32(out, 0);
    putTag(out, "WAVE");
    putTag(out, "LIST");
    put32(out, 3);
    out.insert(out.end(), { 'a', 'b', 'c', 0 });
    putTag(out, "fmt ");
    put32(out, 16);
    put16(out, 1);
    put16(out, channels);
    put32(out, sampleRate);
    put32(out, sampleRate * channels * bits / 8);
    put16(out, channels * bits / 8);
    put16(out, bits);
    putTag(out, "data");
    put32(out, dataSize);
    for (int16_t s : samples) { put16(out, (uint16_t)s); }
    return out;
}

TEST(wav_reads_mono_pcm) {
    Clip clip;
    CHECK(parseWav(wavFile(1, 16, { 0, 16384, -32768 }, 6), clip));
    CHECK(clip.sampleRate == 48000);
    CHECK(clip.samples.size() == 3);
    CHECK_NEAR(clip.samples[1], 0.5, 1e-6);
    CHECK_NEAR(clip.samples[2], -1.0, 1e-6);
}

TEST(wav_mixes_stereo_down) {
    Clip clip;
    CHECK(parseWav(wavFile(2, 16, { 16384, 0, -16384, -16384 }, 8), clip));
    CHECK(clip.samples.size() == 2);
    CHECK_NEAR(clip.samples[0], 0.25, 1e-6);
    CHECK_NEAR(clip.samples[1], -0.5, 1e-6);
}

TEST(wav_without_data_size_reads_to_the_end) {
    Clip clip;
    CHECK(parseWav(wavFile(1, 16, { 1, 2, 3, 4 }, 0), clip));
    CHECK(clip.samples.size() == 4);
}

TEST(wav_rejects_other_formats) {
    Clip clip;
    CHECK(!parseWav(wavFile(1, 8, { 1, 2 }, 4), clip));
    CHECK(!parseWav({ 'n', 'o', 'p', 'e' }, clip));
}

TEST(wav_rejects_sample_rates_no_audio_has) {
    // A damaged header: playback sizes its blocks by the rate and would overrun its buffer
    Clip clip;
    CHECK(!parseWav(wavFile(1, 16, { 1, 2 }, 4, 4000000000u), clip));
    CHECK(!parseWav(wavFile(1, 16, { 1, 2 }, 4, 100), clip));
    CHECK(parseWav(wavFile(1, 16, { 1, 2 }, 4, 8000), clip));
    CHECK(parseWav(wavFile(1, 16, { 1, 2 }, 4, 192000), clip));
}

int main() { return runTests(); }
