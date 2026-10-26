#include <dsp/audio/speech_clarity.h>
#include "check.h"
#include <vector>

using dsp::audio::SpeechClarity;

static const double FS = 48000.0;

static std::vector<float> tone(double freq, double amplitude, double seconds) {
    std::vector<float> x((size_t)(seconds * FS));
    for (size_t i = 0; i < x.size(); i++) { x[i] = (float)(amplitude * std::sin(2.0 * M_PI * freq * i / FS)); }
    return x;
}

static std::vector<float> run(SpeechClarity& clarity, const std::vector<float>& x) {
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); i++) {
        float l = x[i], r = x[i];
        clarity.process(l, r);
        y[i] = l;
    }
    return y;
}

static double rmsDb(const std::vector<float>& x, size_t from, size_t to) {
    double sum = 0.0;
    for (size_t i = from; i < to; i++) { sum += (double)x[i] * x[i]; }
    return 10.0 * std::log10(sum / (double)(to - from));
}

TEST(quiet_and_loud_transmitters_come_out_equally_loud) {
    // A weak aircraft, the squelch closed, then a loud tower: 24dB apart going in
    SpeechClarity clarity;
    clarity.setSamplerate(FS);
    std::vector<float> x = tone(1000.0, 0.03, 2.0);
    std::vector<float> gap(48000, 0.0f);
    std::vector<float> loud = tone(1000.0, 0.5, 2.0);
    x.insert(x.end(), gap.begin(), gap.end());
    x.insert(x.end(), loud.begin(), loud.end());
    std::vector<float> y = run(clarity, x);

    double quiet = rmsDb(y, 48000, 96000);
    double strong = rmsDb(y, 96000 + 48000 + 48000, 96000 + 48000 + 96000);
    if (std::fabs(quiet - strong) >= 2.0) { std::printf("  quiet %.1f dB, strong %.1f dB\n", quiet, strong); }
    CHECK(std::fabs(quiet - strong) < 2.0);
}

TEST(peaks_never_go_above_the_ceiling) {
    // Sudden loud onsets after silence, and a signal well over full scale
    SpeechClarity clarity;
    clarity.setSamplerate(FS);
    std::vector<float> x(24000, 0.0f);
    std::vector<float> burst = tone(2200.0, 1.5, 0.5);
    x.insert(x.end(), burst.begin(), burst.end());
    std::vector<float> y = run(clarity, x);
    float peak = 0.0f;
    for (float v : y) { peak = std::max<float>(peak, std::fabs(v)); }
    CHECK(peak <= SpeechClarity::CEILING + 1e-6f);
}

TEST(peaks_over_the_ceiling_are_not_flattened) {
    // A transmitter getting louder every cycle, as an overmodulated voice does: clamping each
    // sample as it came cut a flat top into every new peak
    SpeechClarity clarity;
    clarity.setSamplerate(FS);
    clarity.setLeveling(false);
    std::vector<float> x((size_t)(0.3 * FS));
    for (size_t i = 0; i < x.size(); i++) { x[i] = (float)((0.5 + 5.0 * i / FS) * std::sin(2.0 * M_PI * 1000.0 * i / FS)); }
    std::vector<float> y = run(clarity, x);
    int flatTops = 0;
    int run = 0;
    for (float v : y) {
        run = (std::fabs(v) >= SpeechClarity::CEILING - 1e-4f) ? run + 1 : 0;
        if (run == 2) { flatTops++; }
    }
    if (flatTops > 0) { std::printf("  %d flat tops\n", flatTops); }
    CHECK(flatTops == 0);
}

TEST(silence_from_the_squelch_stays_silent) {
    SpeechClarity clarity;
    clarity.setSamplerate(FS);
    std::vector<float> x = tone(1000.0, 0.2, 0.5);
    x.resize(x.size() + 48000, 0.0f);
    std::vector<float> y = run(clarity, x);
    // The filters ring out within a few ms, the recorder and the gate see exact silence after
    float tail = 0.0f;
    for (size_t i = 24000 + 4800; i < y.size(); i++) { tail = std::max<float>(tail, std::fabs(y[i])); }
    CHECK(tail < 1e-5f);
}

TEST(level_holds_through_the_gaps_between_overs) {
    // The gain learnt on a transmission is kept while the squelch is closed, the next over
    // doesn't start at full gain and blast
    SpeechClarity clarity;
    clarity.setSamplerate(FS);
    std::vector<float> x = tone(1000.0, 0.3, 1.0);
    x.resize(x.size() + 96000, 0.0f);
    std::vector<float> again = tone(1000.0, 0.3, 0.1);
    x.insert(x.end(), again.begin(), again.end());
    std::vector<float> y = run(clarity, x);
    double before = rmsDb(y, 24000, 48000);
    double after = rmsDb(y, 48000 + 96000 + 480, 48000 + 96000 + 4800);
    CHECK(std::fabs(before - after) < 1.5);
}

TEST(consonants_are_lifted_and_hiss_above_the_voice_cut) {
    // Same level going in, compared after the EQ alone: the leveler would even them out
    SpeechClarity clarity;
    clarity.setSamplerate(FS);
    clarity.setLeveling(false);
    double low = rmsDb(run(clarity, tone(500.0, 0.1, 0.5)), 12000, 24000);
    clarity.reset();
    double presence = rmsDb(run(clarity, tone(2200.0, 0.1, 0.5)), 12000, 24000);
    clarity.reset();
    double hiss = rmsDb(run(clarity, tone(6000.0, 0.1, 0.5)), 12000, 24000);
    CHECK(presence - low > 4.0);
    CHECK(low - hiss > 20.0);
}

TEST(works_at_other_output_rates) {
    SpeechClarity clarity;
    clarity.setSamplerate(44100.0);
    std::vector<float> x(44100);
    for (size_t i = 0; i < x.size(); i++) { x[i] = (float)(0.05 * std::sin(2.0 * M_PI * 1000.0 * i / 44100.0)); }
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); i++) {
        float l = x[i], r = x[i];
        clarity.process(l, r);
        y[i] = l;
    }
    double level = rmsDb(y, 22050, 44100);
    CHECK(std::isfinite(level));
    // The envelope follows peaks, a steady tone's RMS is 3dB below them
    CHECK(std::fabs(level - (SpeechClarity::TARGET_DB - 3.0)) < 1.5);
}

int main() { return runTests(); }
