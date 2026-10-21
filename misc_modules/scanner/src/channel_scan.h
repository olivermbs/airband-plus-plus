#pragma once
#include <algorithm>
#include <cmath>
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

    // Averages groups of `factor` bins (dropping the incomplete last group). Unlike the max
    // reduction used for display, averaging lowers the noise variance, so a channel's peak
    // bin isn't biased upwards by noise.
    inline std::vector<float> averageBins(const float* data, int size, int factor) {
        factor = std::max<int>(1, factor);
        std::vector<float> out(size / factor);
        for (int i = 0; i < (int)out.size(); i++) {
            float sum = 0.0f;
            for (int j = 0; j < factor; j++) {
                sum += data[i * factor + j];
            }
            out[i] = sum / (float)factor;
        }
        return out;
    }

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
    // weighted centroid of the bins within 6dB of the peak. Gives sub-bin precision and
    // finds carriers that are off the nominal channel frequency.
    inline double signalCenter(const Spectrum& spectrum, double freq, double halfWidth, double dcGuard) {
        const float PEAK_RANGE_DB = 6.0f;

        int low = std::max<int>(0, spectrum.binOf(freq - halfWidth));
        int high = std::min<int>(spectrum.width - 1, spectrum.binOf(freq + halfWidth));
        float peak = channelPeak(spectrum, freq, halfWidth, dcGuard);
        if (!std::isfinite(peak)) { return freq; }

        double weightSum = 0.0;
        double freqSum = 0.0;
        for (int i = low; i <= high; i++) {
            if (spectrum.binNearDc(i, dcGuard)) { continue; }
            if (spectrum.data[i] < peak - PEAK_RANGE_DB) { continue; }
            double weight = std::pow(10.0, (spectrum.data[i] - peak) / 10.0);
            double binCenter = spectrum.start + ((double)i + 0.5) * spectrum.binWidth();
            weightSum += weight;
            freqSum += weight * binCenter;
        }

        return freqSum / weightSum;
    }
}
