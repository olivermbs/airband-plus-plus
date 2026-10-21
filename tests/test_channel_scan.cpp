#include <channel_scan.h>
#include "check.h"
#include <random>

using namespace scan;

static const double STEP_25K = 25000.0;
static const double STEP_833 = 25000.0 / 3.0;

TEST(grid_counts_airband_channels) {
    ChannelGrid grid25 = { 118000000.0, 136975000.0, STEP_25K };
    CHECK(grid25.count() == 760);
    CHECK_NEAR(grid25.freq(759), 136975000.0, 0.001);

    ChannelGrid grid833 = { 118000000.0, 136991666.667, STEP_833 };
    CHECK(grid833.count() == 2280);
    CHECK_NEAR(grid833.freq(1), 118008333.333, 0.001);
    CHECK(grid833.indexOf(118016666.667) == 2);
}

TEST(grid_rejects_invalid_ranges) {
    CHECK((ChannelGrid{ 120e6, 118e6, STEP_25K }).count() == 0);
    CHECK((ChannelGrid{ 118e6, 120e6, 0.0 }).count() == 0);
}

static void checkPlan(const ChannelGrid& grid, double usable) {
    auto windows = planWindows(grid, usable);
    CHECK(!windows.empty());
    int expectedFirst = 0;
    for (const auto& w : windows) {
        CHECK(w.first == expectedFirst);
        CHECK(w.last >= w.first);
        for (int i = w.first; i <= w.last; i++) {
            double off = std::fabs(grid.freq(i) - w.center);
            CHECK(off <= usable / 2.0 + 1e-6);
            CHECK(off >= grid.step / 2.0 - 1e-6);
        }
        expectedFirst = w.last + 1;
    }
    CHECK(expectedFirst == grid.count());
}

TEST(windows_cover_every_channel_once_with_dc_between_channels) {
    checkPlan({ 118000000.0, 136975000.0, STEP_25K }, 2.0e6);
    checkPlan({ 118000000.0, 136991666.667, STEP_833 }, 2.0e6);
    checkPlan({ 118000000.0, 118100000.0, STEP_25K }, 2.0e6);
    checkPlan({ 118000000.0, 118000000.0, STEP_25K }, 2.0e6);
}

TEST(windows_use_the_usable_span) {
    auto windows = planWindows({ 118000000.0, 136975000.0, STEP_25K }, 2.0e6);
    CHECK(windows.size() == 10);
}

struct FakeSpectrum {
    std::vector<float> bins;
    Spectrum view;

    FakeSpectrum(double center, double span, int width, float floor, unsigned seed) : bins(width) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> noise(0.0f, 1.5f);
        for (auto& b : bins) { b = floor + noise(rng); }
        view = { bins.data(), width, center - span / 2.0, span, center };
    }

    void addCarrier(double freq, float level) {
        int b = view.binOf(freq);
        bins[b] = level;
    }
};

TEST(channel_peak_finds_carrier_and_ignores_dc_spike) {
    double center = 120012500.0;
    FakeSpectrum s(center, 2.4e6, 1500, -60.0f, 1);
    s.addCarrier(120500000.0, -35.0f);
    s.addCarrier(center, -20.0f); // DC spike

    CHECK_NEAR(channelPeak(s.view, 120500000.0, 8750.0, 3000.0), -35.0f, 0.01f);

    // 8.33kHz channel next to DC: the spike must not leak into it
    float nextToDc = channelPeak(s.view, center + STEP_833 / 2.0, 2900.0, 3000.0);
    CHECK(nextToDc < -50.0f);
}

TEST(noise_floor_ignores_busy_channels) {
    FakeSpectrum s(120000000.0, 2.4e6, 1500, -60.0f, 2);
    for (int i = 0; i < 6; i++) {
        s.addCarrier(119950000.0 + i * 25000.0, -30.0f);
    }
    float floor = noiseFloor(s.view, 120025000.0, 150000.0, 8750.0, 3000.0, 0.3f);
    CHECK_NEAR(floor, -60.8f, 1.0f);
}

TEST(noise_floor_falls_back_to_whole_spectrum_at_edges) {
    FakeSpectrum s(120000000.0, 2.4e6, 1500, -60.0f, 3);
    float floor = noiseFloor(s.view, 118800000.0 + 1.0, 1000.0, 900.0, 3000.0, 0.3f);
    CHECK_NEAR(floor, -60.8f, 1.0f);
}

TEST(pick_requires_confirmation) {
    std::vector<float> snr = { 0, 15, 0, 0 };
    CHECK(pickStrongest(snr, { 0, 1, 0, 0 }, 2, 8.0f) == -1);
    CHECK(pickStrongest(snr, { 0, 2, 0, 0 }, 2, 8.0f) == 1);
}

TEST(pick_takes_highest_snr) {
    std::vector<float> snr = { 12, 0, 30, 0, 20 };
    std::vector<int> hits = { 3, 0, 3, 0, 3 };
    CHECK(pickStrongest(snr, hits, 2, 8.0f) == 2);
}

TEST(pick_ignores_unconfirmed_stronger_channel) {
    std::vector<float> snr = { 12, 40 };
    std::vector<int> hits = { 2, 1 };
    CHECK(pickStrongest(snr, hits, 2, 8.0f) == 0);
}

TEST(pick_lands_on_carrier_not_on_bleed) {
    // Strong signal on channel 3 bleeds into 2 and 4
    std::vector<float> snr = { 0, 0, 10, 25, 11, 0 };
    std::vector<int> hits = { 0, 0, 2, 2, 2, 0 };
    CHECK(pickStrongest(snr, hits, 2, 8.0f) == 3);
}

TEST(signal_center_finds_offset_carrier) {
    FakeSpectrum s(120000000.0, 2.4e6, 1500, -60.0f, 4);
    double binWidth = s.view.binWidth();
    double channel = 120500000.0;

    // Carrier 5kHz above the nominal channel (offset carrier)
    int b = s.view.binOf(channel + 5000.0);
    s.bins[b] = -30.0f;
    double binCenter = s.view.start + (b + 0.5) * binWidth;
    CHECK_NEAR(signalCenter(s.view, channel, 8750.0, 3000.0), binCenter, 1.0);
}

TEST(signal_center_interpolates_between_bins) {
    FakeSpectrum s(120000000.0, 2.4e6, 1500, -60.0f, 5);
    double binWidth = s.view.binWidth();
    int b = s.view.binOf(120500000.0);
    s.bins[b] = -30.0f;
    s.bins[b + 1] = -30.0f;
    double between = s.view.start + (b + 1) * binWidth;
    CHECK_NEAR(signalCenter(s.view, 120500000.0, 8750.0, 3000.0), between, 1.0);
}

TEST(signal_center_ignores_noise_and_dc) {
    double center = 120012500.0;
    FakeSpectrum s(center, 2.4e6, 1500, -60.0f, 6);
    s.addCarrier(center, -20.0f); // DC spike inside the channel window
    int b = s.view.binOf(center + 8000.0);
    s.bins[b] = -35.0f;
    double binCenter = s.view.start + (b + 0.5) * s.view.binWidth();
    CHECK_NEAR(signalCenter(s.view, center + 8000.0, 8750.0, 3000.0), binCenter, 1.0);
}

int main() { return runTests(); }
