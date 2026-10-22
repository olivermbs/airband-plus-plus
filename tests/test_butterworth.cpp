#include <dsp/filter/butterworth.h>
#include "check.h"
#include <vector>

using dsp::filter::ButterworthHighPass;

// Gain in dB of a sine after the filter has settled
static double gainDb(double freq, double cutoff, double samplerate) {
    ButterworthHighPass hp;
    hp.design(cutoff, samplerate);
    double inPower = 0.0, outPower = 0.0;
    int settle = (int)samplerate;
    int count = settle + (int)samplerate;
    for (int i = 0; i < count; i++) {
        float x = std::sin(2.0 * M_PI * freq * i / samplerate);
        float y = hp.process(x, 0);
        if (i < settle) { continue; }
        inPower += x * x;
        outPower += y * y;
    }
    return 10.0 * std::log10(outPower / inPower);
}

TEST(passes_voice) {
    CHECK_NEAR(gainDb(1000.0, 300.0, 48000.0), 0.0, 0.1);
    CHECK_NEAR(gainDb(3000.0, 300.0, 48000.0), 0.0, 0.1);
}

TEST(minus_3db_at_cutoff) {
    CHECK_NEAR(gainDb(300.0, 300.0, 48000.0), -3.0, 0.2);
}

TEST(rejects_hum_and_rumble) {
    CHECK(gainDb(150.0, 300.0, 48000.0) < -30.0);
    CHECK(gainDb(50.0, 300.0, 48000.0) < -80.0);
}

TEST(channels_are_independent) {
    ButterworthHighPass hp;
    hp.design(300.0, 48000.0);
    for (int i = 0; i < 1000; i++) { hp.process(1.0f, 0); }
    // Channel 1 never saw the DC step of channel 0
    CHECK_NEAR(hp.process(0.0f, 1), 0.0f, 1e-9);
}

int main() { return runTests(); }
