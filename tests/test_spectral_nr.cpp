#include <dsp/noise_reduction/spectral_nr.h>
#include "check.h"
#include <random>
#include <vector>

using dsp::noise_reduction::SpectralNoiseReducer;

static const double FS = 48000.0;

static double power(const std::vector<float>& x, int start) {
    double p = 0.0;
    for (size_t i = start; i < x.size(); i++) { p += (double)x[i] * x[i]; }
    return p / (x.size() - start);
}

// Speech-like: a tone that starts and stops, 0.3s on, 0.3s off
static std::vector<float> bursts(int count, double freq, float amplitude) {
    std::vector<float> x(count);
    for (int i = 0; i < count; i++) {
        bool on = std::fmod(i / FS, 0.6) < 0.3;
        x[i] = on ? amplitude * (float)std::sin(2.0 * M_PI * freq * i / FS) : 0.0f;
    }
    return x;
}

// How much of the clean signal is in the output (least squares), and the power of the rest
static void separate(const std::vector<float>& out, const std::vector<float>& clean, int start, double& signalLossDb, double& restPower) {
    double cross = 0.0, cleanPower = 0.0;
    for (size_t i = start; i < out.size(); i++) {
        cross += (double)out[i] * clean[i];
        cleanPower += (double)clean[i] * clean[i];
    }
    double a = cross / cleanPower;
    signalLossDb = -20.0 * std::log10(a);
    restPower = 0.0;
    for (size_t i = start; i < out.size(); i++) {
        double e = out[i] - a * clean[i];
        restPower += e * e;
    }
    restPower /= (out.size() - start);
}

static std::vector<float> run(const std::vector<float>& in) {
    SpectralNoiseReducer nr;
    nr.init(FS);
    std::vector<float> out(in.size());
    // Odd block sizes, like the DSP chain delivers
    for (size_t i = 0; i < in.size(); i += 333) {
        int count = (int)std::min<size_t>(333, in.size() - i);
        nr.process(count, &in[i], &out[i]);
    }
    return out;
}

TEST(noise_is_reduced) {
    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> in((int)(4 * FS));
    for (auto& v : in) { v = noise(rng); }
    auto out = run(in);
    int start = (int)(2 * FS);
    double reduction = 10.0 * std::log10(power(in, start) / power(out, start));
    std::printf("  noise reduced by %.1f dB\n", reduction);
    CHECK(reduction > 8.0);
}

TEST(speech_is_kept_and_noise_reduced) {
    std::mt19937 rng(2);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    int count = (int)(4 * FS);
    // About 10dB above the noise while on
    auto clean = bursts(count, 1000.0, 0.447f);
    std::vector<float> in(count);
    for (int i = 0; i < count; i++) { in[i] = clean[i] + noise(rng); }
    auto out = run(in);

    SpectralNoiseReducer probe;
    probe.init(FS);
    int delay = probe.latency();
    std::vector<float> delayed(count, 0.0f);
    for (int i = delay; i < count; i++) { delayed[i] = clean[i - delay]; }

    double loss, restOut;
    separate(out, delayed, (int)(2 * FS), loss, restOut);
    double restIn = 0.01;
    double reduction = 10.0 * std::log10(restIn / restOut);
    std::printf("  speech loss %.2f dB, noise reduced by %.1f dB\n", loss, reduction);
    CHECK(loss < 1.5);
    CHECK(reduction > 6.0);
}

TEST(clean_speech_passes) {
    int count = (int)(2 * FS);
    auto clean = bursts(count, 700.0, 0.5f);
    auto out = run(clean);
    SpectralNoiseReducer probe;
    probe.init(FS);
    int d = probe.latency();
    double c = 0.0, p = 0.0;
    for (int i = (int)FS; i < count; i++) {
        c += (double)out[i] * clean[i - d];
        p += (double)clean[i - d] * clean[i - d];
    }
    double best = c / p;
    CHECK_NEAR(20.0 * std::log10(best), 0.0, 0.5);
}

int main() { return runTests(); }
