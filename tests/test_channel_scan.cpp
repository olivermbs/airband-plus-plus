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

static void checkListPlan(const std::vector<double>& channels, double usable, double dcClearance) {
    auto windows = planWindows(channels, usable, dcClearance);
    int expectedFirst = 0;
    for (const auto& w : windows) {
        CHECK(w.first == expectedFirst);
        CHECK(w.last >= w.first);
        for (int i = w.first; i <= w.last; i++) {
            double off = std::fabs(channels[i] - w.center);
            CHECK(off <= usable / 2.0 + 1e-6);
            CHECK(off >= dcClearance - 1e-6);
        }
        expectedFirst = w.last + 1;
    }
    CHECK(expectedFirst == (int)channels.size());
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

TEST(average_bins_groups_raw_bins) {
    std::vector<float> raw = { -60, -62, -40, -40, -61, -59, -70 };
    std::vector<float> avg = averageBins(raw.data(), (int)raw.size(), 2);
    CHECK(avg.size() == 3);
    CHECK_NEAR(avg[0], -61.0f, 0.001f);
    CHECK_NEAR(avg[1], -40.0f, 0.001f);
    CHECK_NEAR(avg[2], -60.0f, 0.001f);

    CHECK(averageBins(raw.data(), (int)raw.size(), 1).size() == raw.size());
}

TEST(averaging_keeps_noise_peaks_low) {
    // Max of noise bins in a channel must stay well below the trigger after averaging
    std::mt19937 rng(7);
    std::exponential_distribution<float> power(1.0f);
    std::vector<float> raw(65536);
    for (auto& v : raw) { v = 10.0f * std::log10(power(rng)); }
    std::vector<float> avg = averageBins(raw.data(), (int)raw.size(), 16);
    Spectrum s = { avg.data(), (int)avg.size(), 119e6, 2.4e6, 120.2e6 };
    float worst = 0.0f;
    for (double f = 119.1e6; f < 120.1e6; f += STEP_833) {
        float snr = channelPeak(s, f, 2900.0, 1000.0) - noiseFloor(s, f, 150000.0, 2900.0, 1000.0, 0.3f);
        worst = std::max(worst, snr);
    }
    CHECK(worst < 6.0f);
}

TEST(grid_windows_cover_every_channel_once_with_dc_between_channels) {
    for (double step : { STEP_25K, STEP_833 }) {
        ChannelGrid grid = { 118000000.0, 136990000.0, step };
        checkListPlan(gridChannels(grid), 2.0e6, step / 2.0);
    }
    checkListPlan(gridChannels({ 118000000.0, 118100000.0, STEP_25K }), 2.0e6, STEP_25K / 2.0);
    checkListPlan(gridChannels({ 118000000.0, 118000000.0, STEP_25K }), 2.0e6, STEP_25K / 2.0);
}

TEST(list_windows_cover_bookmarks_clear_of_dc) {
    // LOWK: tower, guard, radar, ATIS
    std::vector<double> lowk = { 118.100e6, 121.500e6, 123.325e6, 126.330e6 };
    checkListPlan(lowk, 2.176e6, 4000.0);
    CHECK(planWindows(lowk, 2.176e6, 4000.0).size() == 3);

    // Two bookmarks closer than the DC clearance to any center between them
    checkListPlan({ 120.000e6, 120.005e6 }, 2.176e6, 4000.0);
    checkListPlan({ 120.000e6 }, 2.176e6, 4000.0);
}

TEST(list_windows_match_grid_windows) {
    ChannelGrid grid = { 118000000.0, 136975000.0, STEP_25K };
    std::vector<double> channels = gridChannels(grid);
    CHECK(channels.size() == 760);
    checkListPlan(channels, 2.0e6, STEP_25K / 2.0);
    CHECK(planWindows(channels, 2.0e6, STEP_25K / 2.0).size() == 10);
}

TEST(list_windows_skip_empty_spectrum) {
    // Nothing between 119 and 135 MHz, no window should be spent there
    std::vector<double> channels = { 118.0e6, 118.5e6, 135.0e6, 135.2e6 };
    CHECK(planWindows(channels, 2.0e6, 4000.0).size() == 2);
}

TEST(nearest_channel_in_list) {
    std::vector<double> channels = { 118.100e6, 121.500e6, 123.325e6 };
    CHECK(nearestChannel(channels, 121.502e6) == 1);
    CHECK(nearestChannel(channels, 100e6) == 0);
    CHECK(nearestChannel(channels, 130e6) == 2);
}

int main() { return runTests(); }
