#include <dsp/noise_reduction/spectrum_noise.h>
#include "check.h"
#include <random>
#include <vector>

using namespace dsp::noise_reduction;

static const double RATE = 2.56e6;
static const int SIZE = 8192;
static const double CENTER = 126.0e6;
static const double BIN = RATE / SIZE;
static const double START = CENTER - RATE / 2.0 - BIN / 2.0;

// Noise in dB per bin, with an optional slope in dB per MHz
static std::vector<float> noise(unsigned seed, float levelDb, float slopeDbPerMHz = 0.0f) {
    std::mt19937 rng(seed);
    std::exponential_distribution<float> power(1.0f);
    std::vector<float> bins(SIZE);
    for (int i = 0; i < SIZE; i++) {
        double f = START + (i + 0.5) * BIN;
        bins[i] = levelDb + slopeDbPerMHz * (float)((f - CENTER) / 1e6) + 10.0f * std::log10(power(rng));
    }
    return bins;
}

static void addTone(std::vector<float>& bins, double freq, float levelDb) {
    int i = (int)std::floor((freq - START) / BIN);
    bins[i] = 10.0f * std::log10(std::pow(10.0f, bins[i] / 10.0f) + std::pow(10.0f, levelDb / 10.0f));
}

TEST(empty_channel_measures_as_noise) {
    auto bins = noise(1, -60.0f);
    ChannelPower p = measureChannel(bins.data(), SIZE, START, RATE, CENTER, 126.325e6, 7000.0);
    CHECK(p.valid);
    CHECK_NEAR(p.channelDb - p.noiseDb, 0.0f, 1.0f);
    // 7kHz of -60dB bins
    CHECK_NEAR(p.noiseDb, -60.0f + 10.0f * std::log10(7000.0f / BIN), 0.5f);
}

TEST(signal_in_channel_does_not_raise_noise) {
    auto bins = noise(2, -60.0f);
    addTone(bins, 126.325e6, -30.0f);
    ChannelPower p = measureChannel(bins.data(), SIZE, START, RATE, CENTER, 126.325e6, 7000.0);
    CHECK_NEAR(p.noiseDb, -60.0f + 10.0f * std::log10(7000.0f / BIN), 0.5f);
    CHECK(p.channelDb - p.noiseDb > 10.0f);
}

TEST(neighbouring_carriers_do_not_raise_noise) {
    auto bins = noise(3, -60.0f);
    for (int k = 1; k <= 5; k++) {
        addTone(bins, 126.325e6 + k * 8333.0, -25.0f);
        addTone(bins, 126.325e6 - k * 8333.0, -25.0f);
    }
    ChannelPower p = measureChannel(bins.data(), SIZE, START, RATE, CENTER, 126.325e6, 7000.0);
    CHECK_NEAR(p.noiseDb, -60.0f + 10.0f * std::log10(7000.0f / BIN), 0.7f);
}

TEST(follows_local_noise_level_on_a_slope) {
    // 10dB per MHz, e.g. towards the edge of the passband
    auto bins = noise(4, -60.0f, 10.0f);
    double freq = 126.5e6;
    ChannelPower p = measureChannel(bins.data(), SIZE, START, RATE, CENTER, freq, 7000.0);
    float local = -60.0f + 10.0f * 0.5f;
    CHECK_NEAR(p.noiseDb, local + 10.0f * std::log10(7000.0f / BIN), 0.7f);
}

TEST(invalid_outside_the_spectrum) {
    auto bins = noise(5, -60.0f);
    CHECK(!measureChannel(bins.data(), SIZE, START, RATE, CENTER, 130.0e6, 7000.0).valid);
}

int main() { return runTests(); }
