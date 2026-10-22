#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp::noise_reduction {
    struct ChannelPower {
        float channelDb; // power within the channel bandwidth
        float noiseDb;   // noise power over the same bandwidth, measured around the channel
        bool valid;
    };

    // Median of noise power relative to its mean, for bins that are each the mean of a number of
    // independent noise powers: ln(2) for a single FFT bin, closer to 1 the more is averaged
    // (Wilson-Hilferty approximation of the gamma distribution's median).
    inline double noiseMedianRatio(double averagedSamples) {
        const double LN2 = 0.693147181;
        if (averagedSamples <= 1.0) { return LN2; }
        double c = 1.0 - 1.0 / (9.0 * averagedSamples);
        return c * c * c;
    }

    // Measures a channel and the noise right around it from an FFT in dB per bin. The noise
    // comes from either side of the channel, leaving out a guard band and DC, as the median
    // power: other signals around don't move it, and it follows the local noise level where the
    // spectrum isn't flat. Bins cover [start + i * binWidth, start + (i + 1) * binWidth).
    // averagedSamples: how many independent noise powers each bin is the mean of, 1 for an FFT.
    inline ChannelPower measureChannel(const float* fft, int size, double start, double span, double dcFreq, double freq, double bandwidth, double averagedSamples = 1.0) {
        const double NOISE_RANGE = 60000.0;
        const double DC_GUARD = 1000.0;
        const double DB_TO_LN = 0.230258509;
        const size_t MIN_REFERENCE_BINS = 16;

        double binWidth = span / (double)size;
        double half = bandwidth / 2.0;
        double guard = half;
        int low = std::max<int>(0, (int)std::floor((freq - half - guard - NOISE_RANGE - start) / binWidth));
        int high = std::min<int>(size - 1, (int)std::floor((freq + half + guard + NOISE_RANGE - start) / binWidth));

        double channel = 0.0;
        int channelBins = 0;
        std::vector<float> reference;
        for (int i = low; i <= high; i++) {
            double offset = std::fabs(start + ((double)i + 0.5) * binWidth - freq);
            float power = (float)std::exp(fft[i] * DB_TO_LN);
            if (offset <= half) {
                channel += power;
                channelBins++;
                continue;
            }
            if (offset < half + guard) { continue; }
            if (std::fabs(start + ((double)i + 0.5) * binWidth - dcFreq) < DC_GUARD + binWidth) { continue; }
            reference.push_back(power);
        }

        if (channelBins == 0 || reference.size() < MIN_REFERENCE_BINS) { return { 0.0f, 0.0f, false }; }

        size_t mid = reference.size() / 2;
        std::nth_element(reference.begin(), reference.begin() + mid, reference.end());
        double noisePerBin = reference[mid] / noiseMedianRatio(averagedSamples);

        return { (float)(10.0 * std::log10(channel)), (float)(10.0 * std::log10(noisePerBin * channelBins)), true };
    }
}
