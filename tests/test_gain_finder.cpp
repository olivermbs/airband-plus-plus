#include <gain_finder.h>
#include "check.h"

using namespace gain;

// The RTL-SDR Blog V4's gains, tenths of dB
static const std::vector<int> GAINS = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };

// Noise floor of a receiver: the ADC's floor, plus the front end's noise growing with the gain
static std::vector<Step> sweep(float adcDb, float frontEndAtZeroDb, int overloadFrom = 1000, bool clipping = false) {
    std::vector<Step> steps;
    for (int g : GAINS) {
        float frontEnd = frontEndAtZeroDb + (float)g / 10.0f;
        float floor = 10.0f * std::log10(std::pow(10.0f, adcDb / 10.0f) + std::pow(10.0f, frontEnd / 10.0f));
        Step s = { g, floor, 0.0, 2 };
        if (g >= overloadFrom) {
            if (clipping) { s.clipped = 1e-3; }
            else {
                s.floorDb += 2.0f * (float)(g - overloadFrom) / 10.0f;
                s.steadyPeaks = 2 + (g - overloadFrom) / 20;
            }
        }
        steps.push_back(s);
    }
    return steps;
}

TEST(clean_receiver_stops_where_more_gain_stops_helping) {
    // Front end noise reaches the ADC's at 20dB of gain, 7dB above it around 28dB
    auto steps = sweep(-60.0f, -80.0f);
    Choice c = choose(steps);
    CHECK(c.overload < 0);
    CHECK(steps[c.step].gain >= 254 && steps[c.step].gain <= 297);
}

TEST(overload_by_a_climbing_floor_is_kept_clear_of) {
    // Lots of front end noise early, then intermodulation from 30dB
    auto steps = sweep(-60.0f, -70.0f, 300);
    Choice c = choose(steps);
    CHECK(c.overload >= 0);
    CHECK(steps[c.step].gain <= 300);
}

TEST(clipping_is_overload) {
    // A weak front end would want all the gain, the ADC clips from 40dB
    auto steps = sweep(-60.0f, -100.0f, 400, true);
    Choice c = choose(steps);
    CHECK(c.overload >= 0);
    CHECK(steps[c.overload].gain == 402);
    CHECK(steps[c.step].gain <= 402 - OVERLOAD_MARGIN);
}

TEST(weak_antenna_gets_the_highest_clean_gain) {
    auto steps = sweep(-60.0f, -120.0f);
    Choice c = choose(steps);
    CHECK(c.step == (int)steps.size() - 1);
}

TEST(spurs_that_stay_are_overload_but_a_passing_signal_isnt) {
    auto steps = sweep(-60.0f, -80.0f);
    // A transmission during one step
    steps[20].steadyPeaks = 6;
    CHECK(choose(steps).overload < 0);
    // Intermodulation from 40dB on
    for (size_t i = 22; i < steps.size(); i++) { steps[i].steadyPeaks = 8; }
    Choice c = choose(steps);
    CHECK(c.overload == 22);
}

TEST(steady_peaks_count_neighbouring_bins_once) {
    std::vector<std::vector<float>> frames(8, std::vector<float>(100, -60.0f));
    std::vector<float> floors(8, -60.0f);
    for (auto& f : frames) {
        f[10] = -40.0f;
        f[11] = -38.0f;
        f[50] = -40.0f;
    }
    // Only in two frames: a transmission
    frames[0][80] = -30.0f;
    frames[1][80] = -30.0f;
    CHECK(countSteadyPeaks(frames, floors, 12.0f, 0.85f) == 2);
}

TEST(median_floor_ignores_the_edges) {
    std::vector<float> bins(1000, -60.0f);
    for (int i = 0; i < 100; i++) {
        bins[i] = -90.0f;
        bins[999 - i] = -90.0f;
    }
    CHECK_NEAR(medianFloor(bins, 0.1f), -60.0f, 0.001f);
}

int main() { return runTests(); }
