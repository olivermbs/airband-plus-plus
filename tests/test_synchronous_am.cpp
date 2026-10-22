#include <dsp/demod/synchronous_am.h>
#include "check.h"
#include <random>
#include <vector>

using dsp::complex_t;
using dsp::demod::SynchronousDetector;

static const double FS = 15000.0;

// AM at a carrier offset, 1kHz tone, with complex noise of the given power (carrier power is 1)
static std::vector<complex_t> amSignal(int count, double offset, double modulation, double noisePower, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, (float)std::sqrt(noisePower / 2.0));
    std::vector<complex_t> x(count);
    for (int i = 0; i < count; i++) {
        double t = i / FS;
        double env = 1.0 + modulation * std::sin(2.0 * M_PI * 1000.0 * t);
        double ph = 2.0 * M_PI * offset * t + 0.7;
        x[i] = { (float)(env * std::cos(ph)) + noise(rng), (float)(env * std::sin(ph)) + noise(rng) };
    }
    return x;
}

static std::vector<float> envelope(const std::vector<complex_t>& x) {
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); i++) { y[i] = std::sqrt(x[i].re * x[i].re + x[i].im * x[i].im); }
    return y;
}

static std::vector<float> synchronous(const std::vector<complex_t>& x) {
    SynchronousDetector d;
    d.init(FS);
    std::vector<float> y(x.size());
    d.process((int)x.size(), x.data(), y.data());
    return y;
}

// SNR of the 1kHz tone in a demodulated output against everything else, after settling:
// what a listener hears. Phase jitter of the PLL that doesn't affect the audio isn't counted.
static double toneSNR(const std::vector<float>& y) {
    int start = (int)(0.5 * FS);
    int n = (int)y.size() - start;
    double mean = 0.0;
    for (int i = 0; i < n; i++) { mean += y[start + i]; }
    mean /= n;
    double re = 0.0, im = 0.0, total = 0.0;
    for (int i = 0; i < n; i++) {
        double v = y[start + i] - mean;
        double ph = 2.0 * M_PI * 1000.0 * (start + i) / FS;
        re += v * std::cos(ph);
        im += v * std::sin(ph);
        total += v * v;
    }
    double tone = 2.0 * (re * re + im * im) / n;
    return 10.0 * std::log10(tone / (total - tone));
}

TEST(locks_onto_offset_carriers) {
    for (double offset : { -1500.0, -300.0, 0.0, 800.0, 1500.0 }) {
        auto x = amSignal((int)(0.3 * FS), offset, 0.5, 0.01, 1);
        SynchronousDetector d;
        d.init(FS);
        std::vector<float> y(x.size());
        d.process((int)x.size(), x.data(), y.data());
        double found = d.frequency() * FS / (2.0 * M_PI);
        if (std::fabs(found - offset) > 5.0) { std::printf("  offset %.0f: found %.1f\n", offset, found); }
        CHECK_NEAR(found, offset, 5.0);
    }
}

TEST(at_least_as_good_as_envelope_and_better_when_weak) {
    int count = (int)(3.0 * FS);
    for (double noisePower : { 0.1, 0.5, 1.0 }) {
        auto x = amSignal(count, 800.0, 0.5, noisePower, 2);
        double env = toneSNR(envelope(x));
        double sync = toneSNR(synchronous(x));
        std::printf("  noise %.1f: envelope %.1f dB, synchronous %.1f dB\n", noisePower, env, sync);
        CHECK(sync >= env - 0.3);
        if (noisePower >= 1.0) { CHECK(sync >= env + 1.0); }
    }
}

TEST(stays_finite_on_noise_only) {
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::vector<complex_t> x((int)FS);
    for (auto& v : x) { v = { noise(rng), noise(rng) }; }
    auto y = synchronous(x);
    for (float v : y) { CHECK(std::isfinite(v)); }
}

TEST(holds_frequency_through_silence) {
    auto x = amSignal((int)(0.5 * FS), 800.0, 0.5, 0.01, 4);
    SynchronousDetector d;
    d.init(FS);
    std::vector<float> y(x.size());
    d.process((int)x.size(), x.data(), y.data());
    std::vector<complex_t> silence((int)(0.5 * FS), complex_t{ 0.0f, 0.0f });
    std::vector<float> z(silence.size());
    d.process((int)silence.size(), silence.data(), z.data());
    CHECK_NEAR(d.frequency() * FS / (2.0 * M_PI), 800.0, 5.0);
}

int main() { return runTests(); }
