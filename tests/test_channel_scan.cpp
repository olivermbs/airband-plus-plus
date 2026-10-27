#include <channel_scan.h>
#include "check.h"
#include <limits>
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

// An AM transmitter in the spectrum: the carrier with voice sidebands either side of it
static void addAmSignal(FakeSpectrum& s, double carrier, float carrierLevel, float sidebandLevel) {
    double binWidth = s.view.binWidth();
    for (double offset = 300.0; offset <= 3000.0; offset += binWidth) {
        s.bins[s.view.binOf(carrier - offset)] = sidebandLevel;
        s.bins[s.view.binOf(carrier + offset)] = sidebandLevel;
    }
    s.addCarrier(carrier, carrierLevel);
}

TEST(offset_carrier_between_833_channels_belongs_to_the_nearest) {
    // Offset carrier systems put a 25kHz channel's carriers a few kHz off, here 118.100 is
    // heard on 118.1035, beyond a third of the 8.33kHz spacing. 312.5Hz bins as with an
    // 8192 point FFT at 2.56MS/s.
    double center = 118000000.0;
    FakeSpectrum s(center, 2.56e6, 8192, -60.0f, 13);
    double carrier = 118103500.0;
    addAmSignal(s, carrier, -30.0f, -45.0f);
    double carrierBin = s.view.start + (s.view.binOf(carrier) + 0.5) * s.view.binWidth();
    double halfWidth = channelHalfWidth(STEP_833);

    CHECK_NEAR(channelPeak(s.view, 118100000.0, halfWidth, 1000.0), -30.0f, 0.01f);
    double found = signalCenter(s.view, 118100000.0, halfWidth, 1000.0);
    CHECK_NEAR(found, carrierBin, 1.0);

    std::vector<double> channels = gridChannels({ 118000000.0, 118200000.0, STEP_833 });
    CHECK_NEAR(channels[nearestChannel(channels, found)], 118100000.0, 0.001);
}

TEST(neighbouring_833_channels_keep_their_own_carriers) {
    // Two transmitters on adjacent channels, each is located on its own channel
    double center = 118000000.0;
    FakeSpectrum s(center, 2.56e6, 8192, -60.0f, 14);
    addAmSignal(s, 118100000.0, -30.0f, -45.0f);
    addAmSignal(s, 118108333.3, -25.0f, -40.0f);
    double halfWidth = channelHalfWidth(STEP_833);
    CHECK_NEAR(signalCenter(s.view, 118100000.0, halfWidth, 1000.0), 118100000.0, s.view.binWidth());
    CHECK_NEAR(signalCenter(s.view, 118108333.3, halfWidth, 1000.0), 118108333.3, s.view.binWidth());
}

TEST(signal_center_takes_the_strongest_of_two_carriers_in_a_channel) {
    // The aircraft on the channel and the ground station on its offset carrier, both keyed:
    // the centre is the stronger one, not a point between them
    FakeSpectrum s(118000000.0, 2.56e6, 8192, -60.0f, 15);
    s.addCarrier(118100000.0, -32.0f);
    s.addCarrier(118103500.0, -30.0f);
    double strongerBin = s.view.start + (s.view.binOf(118103500.0) + 0.5) * s.view.binWidth();
    CHECK_NEAR(signalCenter(s.view, 118100000.0, channelHalfWidth(STEP_833), 1000.0), strongerBin, 1.0);
}

TEST(offset_carriers_belong_to_their_25k_channel) {
    // 118.100 heard on its offset carriers, which are nearest to 8.33kHz neighbours
    CHECK_NEAR(owningChannel(118105000.0, 118108333.3, STEP_833), 118100000.0, 0.001);
    CHECK_NEAR(owningChannel(118096000.0, 118100000.0, STEP_833), 118100000.0, 0.001);
    // The outer carriers of a 3 or 4 carrier system, LOWK tower on 118.1074, lie within a kHz
    // of an 8.33kHz neighbour, further off than an 8.33kHz radio may be
    CHECK_NEAR(owningChannel(118107400.0, 118108333.3, STEP_833), 118100000.0, 0.001);
    CHECK_NEAR(owningChannel(118107500.0, 118108333.3, STEP_833), 118100000.0, 0.001);
    CHECK_NEAR(owningChannel(118092500.0, 118091666.7, STEP_833), 118100000.0, 0.001);
    // An 8.33kHz station within its tolerance (5ppm, 0.6kHz) keeps its channel
    CHECK_NEAR(owningChannel(118108900.0, 118108333.3, STEP_833), 118108333.3, 0.001);
    CHECK_NEAR(owningChannel(118107700.0, 118108333.3, STEP_833), 118108333.3, 0.001);
    // On the 25kHz grid the nearest channel is always right
    CHECK_NEAR(owningChannel(118105000.0, 118100000.0, STEP_25K), 118100000.0, 0.001);
    CHECK(carrierSearchHalfWidth(true, STEP_833) > 7500.0);
    CHECK_NEAR(carrierSearchHalfWidth(false, STEP_833), STEP_833 / 2.0, 0.001);
}

TEST(bookmarks_on_25k_channels_reach_their_offset_carriers) {
    // Alone, a 25kHz channel reaches its outer offset carriers, an 8.33kHz one its own cell
    CHECK(bookmarkHalfWidth(118100000.0, 0.0, 0.0, STEP_833) > 7500.0);
    CHECK_NEAR(bookmarkHalfWidth(132766666.7, 0.0, 0.0, STEP_833), STEP_833 / 2.0, 0.001);
    // Never over half the way to the next bookmark
    CHECK_NEAR(bookmarkHalfWidth(118100000.0, 1e9, 10000.0, STEP_833), 5000.0, 0.001);
    CHECK_NEAR(bookmarkHalfWidth(118100000.0, 6000.0, 1e9, STEP_833), 3000.0, 0.001);
}

TEST(data_channels_are_known_exactly) {
    CHECK(isDataChannel(131725000.0));
    CHECK(isDataChannel(136975000.0));
    // On the 8.33kHz grid, as the scanner computes it
    CHECK(isDataChannel(gridChannels({ 118000000.0, 136991666.667, STEP_833 })[1647]));
    CHECK(!isDataChannel(131733333.3));
    CHECK(!isDataChannel(121500000.0));
    // VDL2 spills into the 8.33kHz channels next to it, ACARS doesn't
    CHECK(isDataChannel(136816666.7));
    CHECK(isDataChannel(136833333.3));
    CHECK(isDataChannel(136966666.7));
    CHECK(!isDataChannel(136800000.0));
    CHECK(!isDataChannel(131716666.7));
}

TEST(bookmark_names_match_within_the_tolerance) {
    std::vector<std::pair<double, std::string>> bookmarks = { { 118100000.0, "TWR" }, { 118108333.3, "APP" }, { 121500000.0, "Guard" } };
    CHECK(bookmarkAt(bookmarks, 118100000.0, 500.0) == "TWR");
    CHECK(bookmarkAt(bookmarks, 118108333.3, 500.0) == "APP");
    CHECK(bookmarkAt(bookmarks, 118104000.0, 500.0).empty());
    CHECK(bookmarkAt({}, 118100000.0, 500.0).empty());
}

TEST(auto_skips_grow_longer_except_for_priority) {
    CHECK(autoSkipMinutes(0, false) == 2);
    CHECK(autoSkipMinutes(1, false) == 5);
    CHECK(autoSkipMinutes(3, false) == 30);
    CHECK(autoSkipMinutes(9, false) == 30);
    CHECK(autoSkipMinutes(9, true) == 2);
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

TEST(detection_average_is_linear_over_bins_and_frames) {
    DetectionAverage d;
    std::vector<float> frame1 = { 0.0f, 0.0f, 10.0f, 10.0f };
    std::vector<float> frame2 = { 0.0f, 0.0f, 0.0f, 0.0f };
    d.add(frame1.data(), 4, 2);
    CHECK(d.bins().size() == 2);
    CHECK_NEAR(d.bins()[0], 0.0f, 0.001f);
    CHECK_NEAR(d.bins()[1], 10.0f, 0.001f);
    d.add(frame2.data(), 4, 2);
    CHECK(d.frameCount() == 2);
    CHECK_NEAR(d.bins()[1], 10.0f * std::log10(5.5f), 0.001f);
    d.reset();
    d.add(frame2.data(), 4, 2);
    CHECK_NEAR(d.bins()[1], 0.0f, 0.001f);
}

// Detection SNR of every channel in a window, as the scanner sees it after averaging
// `frames` FFT frames of noise, optionally with a carrier added
static std::vector<float> windowSNRs(int fftSize, double sampleRate, double step, int frames, unsigned seed, double carrierFreq = 0.0, float carrierBinSNR = 0.0f) {
    std::mt19937 rng(seed);
    std::exponential_distribution<float> power(1.0f);
    double center = 120e6;
    double rawBin = sampleRate / fftSize;
    double start = center - sampleRate / 2.0 - rawBin / 2.0;
    int factor = detectionAveraging(step, rawBin);

    DetectionAverage detection;
    std::vector<float> raw(fftSize);
    for (int f = 0; f < frames; f++) {
        for (auto& v : raw) { v = 10.0f * std::log10(power(rng)); }
        if (carrierFreq > 0.0) {
            int b = (int)std::floor((carrierFreq - start) / rawBin);
            raw[b] = 10.0f * std::log10(std::pow(10.0f, carrierBinSNR / 10.0f) + power(rng));
        }
        detection.add(raw.data(), fftSize, factor);
    }

    const std::vector<float>& bins = detection.bins();
    Spectrum s = { bins.data(), (int)bins.size(), start, rawBin * factor * bins.size(), center };
    std::vector<float> snrs;
    double halfWidth = channelHalfWidth(step);
    for (double f = center - 1.0e6; f < center + 1.0e6; f += step) {
        snrs.push_back(channelPeak(s, f, halfWidth, s.binWidth() * 1.5) - noiseFloor(s, f, 150000.0, halfWidth, s.binWidth() * 1.5, 0.3f));
    }
    return snrs;
}

TEST(noise_stays_below_trigger_for_every_fft_size) {
    for (int fftSize : { 8192, 16384, 32768, 65536, 131072 }) {
        for (double step : { STEP_833, STEP_25K }) {
            std::vector<float> snrs = windowSNRs(fftSize, 2.56e6, step, 2, 11);
            float worst = *std::max_element(snrs.begin(), snrs.end());
            if (worst >= 6.0f) { std::printf("  fft %d step %.0f: noise reaches %.1f dB\n", fftSize, step, worst); }
            CHECK(worst < 6.0f);
        }
    }
}

TEST(weak_narrow_carrier_is_detected_for_every_fft_size) {
    // A carrier 15dB above the noise in a 312Hz bin (8192 point FFT at 2.56MS/s), the
    // same carrier is proportionally stronger in the narrower bins of larger FFTs
    for (int fftSize : { 8192, 16384, 32768, 65536, 131072 }) {
        float binSNR = 15.0f + 10.0f * std::log10((float)fftSize / 8192.0f);
        double carrier = 120e6 + 10 * STEP_833 + 900.0;
        std::vector<float> snrs = windowSNRs(fftSize, 2.56e6, STEP_833, 2, 12, carrier, binSNR);
        int channel = (int)std::lround((carrier - (120e6 - 1.0e6)) / STEP_833);
        if (snrs[channel] < 8.0f) { std::printf("  fft %d: carrier only %.1f dB\n", fftSize, snrs[channel]); }
        CHECK(snrs[channel] >= 8.0f);
    }
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

// One frame of noise around a carrier, optionally with voice-like energy beside it up to voiceEnd
static std::vector<float> carrierFrame(std::mt19937& rng, const Spectrum& layout, double carrier, float carrierSNR, float sidebandSNR, double voiceEnd) {
    std::exponential_distribution<float> power(1.0f);
    std::vector<float> bins(layout.width);
    for (int i = 0; i < layout.width; i++) {
        double f = layout.start + (i + 0.5) * layout.binWidth();
        float p = power(rng);
        double off = std::fabs(f - carrier);
        if (off < layout.binWidth() / 2.0) { p += std::pow(10.0f, carrierSNR / 10.0f); }
        else if (off > 300.0 && off < voiceEnd) { p += std::pow(10.0f, sidebandSNR / 10.0f) * power(rng); }
        bins[i] = 10.0f * std::log10(p);
    }
    return bins;
}

static float averagedModulation(float sidebandSNR, unsigned seed, int size = 8192, double voiceEnd = 3000.0) {
    std::mt19937 rng(seed);
    double center = 132.0e6, rate = 2.56e6;
    double rawBin = rate / size;
    Spectrum layout = { nullptr, size, center - rate / 2.0 - rawBin / 2.0, rate, center };
    double carrier = 132.6e6 + rawBin * 0.5;
    ModulationMeter meter;
    for (int frame = 0; frame < 120; frame++) {
        std::vector<float> bins = carrierFrame(rng, layout, carrier, 15.0f, sidebandSNR, voiceEnd);
        Spectrum s = layout;
        s.data = bins.data();
        meter.add(s, carrier);
    }
    return meter.sidebandToNoiseDb();
}

TEST(modulation_meter_tells_carriers_from_voice) {
    // A bare carrier (spur, birdie): nothing beside it
    CHECK(averagedModulation(-100.0f, 13, 32768) < 0.3f);
    // Weak voice: sideband energy 6dB under the noise per bin is still clearly there on average
    CHECK(averagedModulation(-6.0f, 14, 32768) > 0.5f);
    CHECK(averagedModulation(3.0f, 15, 32768) > 3.0f);
}

TEST(modulation_meter_no_verdict_from_bins_too_coarse_for_voice) {
    // Klagenfurt Tower: clear voice, but its energy is below 1.2kHz. At 312Hz bins the bins beside
    // the carrier are left out up to 1.56kHz, all the meter sees is noise and the channel was
    // skipped as an unmodulated carrier 4s into every reception.
    CHECK(averagedModulation(3.0f, 16, 8192, 1200.0) > 0.3f);
    // Fine enough bins still tell a bare carrier from voice
    CHECK(averagedModulation(-100.0f, 17, 32768, 1200.0) < 0.3f);
    CHECK(averagedModulation(3.0f, 18, 32768, 1200.0) > 0.3f);
}

TEST(grid_count_never_overflows) {
    // Ranges from a hand edited config: too many channels to count in an int, or no number at all
    CHECK((ChannelGrid{ 0.0, 1e18, 1.0 }.count()) == std::numeric_limits<int>::max());
    CHECK((ChannelGrid{ NAN, 1e6, 1.0 }.count()) == 0);
    CHECK((ChannelGrid{ 118e6, 118.1e6, 25e3 }.count()) == 5);
}

int main() { return runTests(); }
