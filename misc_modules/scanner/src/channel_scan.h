#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// Channel scanning logic, independent of the GUI so it can be tested
namespace scan {
    struct ChannelGrid {
        double start;
        double stop;
        double step;

        int count() const {
            if (step <= 0.0 || stop < start) { return 0; }
            return (int)std::floor((stop - start) / step + 1e-6) + 1;
        }

        double freq(int i) const { return start + (double)i * step; }

        int indexOf(double freq) const { return (int)std::lround((freq - start) / step); }
    };

    struct Window {
        double center;
        int first;
        int last;
    };

    inline std::vector<double> gridChannels(const ChannelGrid& grid) {
        std::vector<double> channels(grid.count());
        for (int i = 0; i < (int)channels.size(); i++) {
            channels[i] = grid.freq(i);
        }
        return channels;
    }

    // Index of the channel closest to a frequency, in a sorted list
    inline int nearestChannel(const std::vector<double>& channels, double freq) {
        auto it = std::lower_bound(channels.begin(), channels.end(), freq);
        if (it == channels.end()) { return (int)channels.size() - 1; }
        if (it == channels.begin()) { return 0; }
        int i = (int)(it - channels.begin());
        return (freq - channels[i - 1] < channels[i] - freq) ? i - 1 : i;
    }

    // Splits a sorted channel list into tuning windows no wider than the usable span. Each
    // window reaches as far up as it can while keeping the DC spike at least dcClearance away
    // from every channel, and stretches without channels are skipped entirely.
    inline std::vector<Window> planWindows(const std::vector<double>& channels, double usableSpan, double dcClearance) {
        const double EPSILON = 1e-3;
        std::vector<Window> windows;
        int count = (int)channels.size();
        double half = usableSpan / 2.0;

        int first = 0;
        while (first < count) {
            // Highest center that still covers the first channel, moved down until DC is clear
            double center = channels[first] + half;
            bool moved = true;
            while (moved && center > channels[first] - half) {
                moved = false;
                for (int i = first; i < count && channels[i] <= center + half; i++) {
                    if (std::fabs(channels[i] - center) < dcClearance - EPSILON) {
                        center = channels[i] - dcClearance;
                        moved = true;
                        break;
                    }
                }
            }

            int last = first;
            while (last + 1 < count && channels[last + 1] <= center + half + EPSILON) {
                last++;
            }
            windows.push_back({ center, first, last });
            first = last + 1;
        }

        return windows;
    }

    struct Spectrum {
        const float* data;
        int width;
        double start;
        double span;
        double dcFreq;

        double binWidth() const { return span / (double)width; }

        int binOf(double freq) const { return (int)std::floor((freq - start) / binWidth()); }

        bool binOverlaps(int bin, double low, double high) const {
            double binLow = start + (double)bin * binWidth();
            return binLow < high && binLow + binWidth() > low;
        }

        bool binNearDc(int bin, double dcGuard) const {
            return binOverlaps(bin, dcFreq - dcGuard, dcFreq + dcGuard);
        }
    };

    // Every frequency belongs to the nearest channel, so no carrier gets lost between two. Offset
    // carrier systems put a channel's carriers several kHz off its frequency, on the 8.33kHz
    // grid that is beyond a third of the spacing.
    inline double channelHalfWidth(double step) {
        return step / 2.0;
    }

    // The channel a carrier belongs to. Offset carrier systems put a 25kHz channel's ground
    // transmitters up to 7.5kHz off it, far more than an 8.33kHz radio strays (they are held to
    // about 1ppm, 0.1kHz). So on the 8.33kHz grid a carrier clearly off its nearest channel is
    // one of these, and belongs to the 25kHz channel within reach, not to an 8.33kHz neighbour.
    inline double owningChannel(double carrier, double nearest, double step) {
        const double GRID_833 = 25000.0 / 3.0;
        const double CHANNEL_25K = 25000.0;
        const double TOLERANCE_833 = 1500.0;
        const double OFFSET_REACH = 8000.0;

        if (std::fabs(step - GRID_833) > 1.0) { return nearest; }
        if (std::fabs(carrier - nearest) <= TOLERANCE_833) { return nearest; }
        double channel = std::round(carrier / CHANNEL_25K) * CHANNEL_25K;
        return (std::fabs(carrier - channel) <= OFFSET_REACH) ? channel : nearest;
    }

    // Where carriers of a channel are looked for while listening to it: the whole channel, and
    // the reach of its offset carriers when it is a 25kHz channel heard on one of them
    inline double carrierSearchHalfWidth(bool offsetCarrier, double step) {
        const double OFFSET_REACH = 8000.0;
        return offsetCarrier ? OFFSET_REACH : channelHalfWidth(step);
    }

    // Name of the bookmark on a frequency, empty if there is none within the tolerance
    inline std::string bookmarkAt(const std::vector<std::pair<double, std::string>>& bookmarks, double freq, double tolerance) {
        const std::string* best = nullptr;
        double bestOffset = tolerance;
        for (const auto& [bookmarkFreq, name] : bookmarks) {
            double offset = std::fabs(bookmarkFreq - freq);
            if (offset > bestOffset) { continue; }
            best = &name;
            bestOffset = offset;
        }
        return best ? *best : "";
    }

    // Minutes a channel is skipped automatically, longer each time it happens again. Priority
    // channels never go beyond the first step, something important may start there any time.
    inline int autoSkipMinutes(int strikes, bool priority) {
        const int STEPS[] = { 2, 5, 10, 30 };
        const int LAST = (int)(sizeof(STEPS) / sizeof(STEPS[0])) - 1;
        if (priority) { return STEPS[0]; }
        return STEPS[std::clamp<int>(strikes, 0, LAST)];
    }

    // How many raw FFT bins to average per detection bin: about an eighth of the channel step,
    // and never so few that the noise of a single bin dominates
    inline int detectionAveraging(double step, double rawBinWidth) {
        const int MIN_AVERAGED_BINS = 4;
        return std::max<int>(MIN_AVERAGED_BINS, (int)std::lround(step / 8.0 / rawBinWidth));
    }

    // Averages the power of FFT frames over groups of bins and over consecutive frames.
    // A bin of a single frame of noise varies by about 5.6dB, so without averaging the
    // strongest bin of a channel easily looks like a signal. Averaging is done on linear
    // power, averaging dB values would dilute a narrow carrier into the noise.
    class DetectionAverage {
    public:
        void reset() { frames = 0; }

        void add(const float* db, int size, int factor) {
            int count = size / factor;
            if (frames == 0 || (int)sum.size() != count) {
                sum.assign(count, 0.0f);
                frames = 0;
            }

            for (int i = 0; i < count; i++) {
                float power = 0.0f;
                for (int j = 0; j < factor; j++) {
                    power += std::exp(db[i * factor + j] * DB_TO_LN);
                }
                sum[i] += power / (float)factor;
            }
            frames++;

            out.resize(count);
            for (int i = 0; i < count; i++) {
                out[i] = 10.0f * std::log10(sum[i] / (float)frames);
            }
        }

        int frameCount() const { return frames; }
        const std::vector<float>& bins() const { return out; }

    private:
        static constexpr float DB_TO_LN = 0.230258509f; // ln(10) / 10

        std::vector<float> sum;
        std::vector<float> out;
        int frames = 0;
    };

    // Measures the energy beside a carrier relative to the noise, accumulated over frames.
    // Voice puts energy a few hundred Hz to 3kHz either side of the carrier, an unmodulated
    // carrier (a spur, a birdie, a stuck transmitter) leaves only noise there.
    class ModulationMeter {
    public:
        void reset() {
            sideband = 0.0;
            noise = 0.0;
            frames = 0;
        }

        void add(const Spectrum& s, double carrier) {
            // The FFT window spreads a carrier over a few bins, those must not count as modulation
            double sidebandStart = std::max<double>(MIN_SIDEBAND_OFFSET, CARRIER_BINS * s.binWidth());

            double sidebandSum = 0.0;
            int sidebandCount = 0;
            std::vector<float> reference;
            int low = std::max<int>(0, s.binOf(carrier - NOISE_END));
            int high = std::min<int>(s.width - 1, s.binOf(carrier + NOISE_END));
            for (int i = low; i <= high; i++) {
                double offset = std::fabs(s.start + ((double)i + 0.5) * s.binWidth() - carrier);
                float power = std::exp(s.data[i] * DB_TO_LN);
                if (offset >= sidebandStart && offset <= SIDEBAND_END) {
                    sidebandSum += power;
                    sidebandCount++;
                }
                else if (offset >= NOISE_START) {
                    reference.push_back(power);
                }
            }
            if (sidebandCount == 0 || reference.size() < 16) { return; }

            // Median of exponentially distributed noise power is ln(2) times its mean, and
            // unlike the mean it ignores other signals in the reference range
            size_t mid = reference.size() / 2;
            std::nth_element(reference.begin(), reference.begin() + mid, reference.end());
            sideband += sidebandSum / sidebandCount;
            noise += reference[mid] / LN2;
            frames++;
        }

        int frameCount() const { return frames; }

        // Unknown counts as modulated, nothing should be skipped without evidence
        float sidebandToNoiseDb() const {
            if (frames == 0) { return INFINITY; }
            return 10.0f * std::log10(sideband / noise);
        }

    private:
        static constexpr float DB_TO_LN = 0.230258509f;
        static constexpr double LN2 = 0.693147181;
        static constexpr double MIN_SIDEBAND_OFFSET = 400.0;
        static constexpr double CARRIER_BINS = 5.0;
        static constexpr double SIDEBAND_END = 3000.0;
        static constexpr double NOISE_START = 10000.0;
        static constexpr double NOISE_END = 60000.0;

        double sideband = 0.0;
        double noise = 0.0;
        int frames = 0;
    };

    // Strongest bin within the channel, ignoring the bins around DC
    inline float channelPeak(const Spectrum& spectrum, double freq, double halfWidth, double dcGuard) {
        int low = std::max<int>(0, spectrum.binOf(freq - halfWidth));
        int high = std::min<int>(spectrum.width - 1, spectrum.binOf(freq + halfWidth));

        float peak = -INFINITY;
        for (int i = low; i <= high; i++) {
            if (spectrum.binNearDc(i, dcGuard)) { continue; }
            peak = std::max<float>(peak, spectrum.data[i]);
        }

        return peak;
    }

    // Low quantile of the bins around a channel. Busy channels nearby only occupy a few
    // bins, so the quantile stays on the noise. Falls back to the whole spectrum if the
    // neighbourhood is too small, e.g. at the edges.
    inline float noiseFloor(const Spectrum& spectrum, double freq, double radius, double exclude, double dcGuard, float quantile) {
        const size_t MIN_BINS = 16;

        auto collect = [&](int low, int high) {
            std::vector<float> values;
            for (int i = std::max<int>(0, low); i <= std::min<int>(spectrum.width - 1, high); i++) {
                if (spectrum.binNearDc(i, dcGuard)) { continue; }
                if (spectrum.binOverlaps(i, freq - exclude, freq + exclude)) { continue; }
                values.push_back(spectrum.data[i]);
            }
            return values;
        };

        std::vector<float> values = collect(spectrum.binOf(freq - radius), spectrum.binOf(freq + radius));
        if (values.size() < MIN_BINS) {
            values = collect(0, spectrum.width - 1);
        }
        if (values.empty()) { return -INFINITY; }

        size_t n = (size_t)(quantile * (float)(values.size() - 1));
        std::nth_element(values.begin(), values.begin() + n, values.end());
        return values[n];
    }

    // Picks the confirmed detection with the highest SNR. A strong signal bleeding into
    // the adjacent channels is always strongest on its own channel, so this also avoids
    // stopping next to it.
    inline int pickStrongest(const std::vector<float>& snr, const std::vector<int>& hits, int confirmHits, float trigger) {
        int best = -1;
        for (int i = 0; i < (int)snr.size(); i++) {
            if (hits[i] < confirmHits || snr[i] < trigger) { continue; }
            if (best < 0 || snr[i] > snr[best]) { best = i; }
        }
        return best;
    }

    // Frequency of the middle of the strongest signal within the channel: the power
    // weighted centroid of the bins within 6dB of the peak around it. Gives sub-bin precision
    // and finds carriers that are off the nominal channel frequency. Only the peak's own bins
    // count, a second carrier in the channel would pull the centre between the two.
    inline double signalCenter(const Spectrum& spectrum, double freq, double halfWidth, double dcGuard) {
        const float PEAK_RANGE_DB = 6.0f;

        int low = std::max<int>(0, spectrum.binOf(freq - halfWidth));
        int high = std::min<int>(spectrum.width - 1, spectrum.binOf(freq + halfWidth));
        int peakBin = -1;
        for (int i = low; i <= high; i++) {
            if (spectrum.binNearDc(i, dcGuard)) { continue; }
            if (peakBin < 0 || spectrum.data[i] > spectrum.data[peakBin]) { peakBin = i; }
        }
        if (peakBin < 0 || !std::isfinite(spectrum.data[peakBin])) { return freq; }
        float peak = spectrum.data[peakBin];

        auto belongsToPeak = [&](int i) { return !spectrum.binNearDc(i, dcGuard) && spectrum.data[i] >= peak - PEAK_RANGE_DB; };
        int first = peakBin;
        while (first > low && belongsToPeak(first - 1)) { first--; }
        int last = peakBin;
        while (last < high && belongsToPeak(last + 1)) { last++; }

        double weightSum = 0.0;
        double freqSum = 0.0;
        for (int i = first; i <= last; i++) {
            double weight = std::pow(10.0, (spectrum.data[i] - peak) / 10.0);
            double binCenter = spectrum.start + ((double)i + 0.5) * spectrum.binWidth();
            weightSum += weight;
            freqSum += weight * binCenter;
        }

        return freqSum / weightSum;
    }
}
