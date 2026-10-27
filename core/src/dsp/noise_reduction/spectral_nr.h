#pragma once
#include <fftw3.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace dsp::noise_reduction {
    // Audio noise reduction for one channel. Each FFT band is turned down according to its own
    // signal to noise ratio. The noise of each band is its smoothed power at the quietest moments,
    // so it is learnt during the pauses of speech. The gain is Ephraim and Malah's log-MMSE, which
    // estimates the log spectrum of the speech and leaves it more of its consonants than a Wiener
    // gain; on airband transmissions with receiver noise it scored best for intelligibility
    // (STOI) of the variants tried. Its a priori SNR is smoothed over time (decision directed)
    // and the gain never goes below a floor, which avoids the warbling "musical noise".
    // Overlapping frames (sqrt-Hann windows, 50% overlap) are added back together, the latency is
    // one frame.
    class SpectralNoiseReducer {
    public:
        SpectralNoiseReducer() {}

        ~SpectralNoiseReducer() { destroy(); }

        SpectralNoiseReducer(const SpectralNoiseReducer&) = delete;
        SpectralNoiseReducer& operator=(const SpectralNoiseReducer&) = delete;

        void init(double samplerate) {
            destroy();
            // About 10ms frames
            size = 256;
            while (size < samplerate * 0.0107) { size *= 2; }
            hop = size / 2;
            bins = size / 2 + 1;
            framesPerSecond = samplerate / hop;

            window.resize(size);
            for (int i = 0; i < size; i++) { window[i] = std::sqrt(0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / size)); }

            time = fftwf_alloc_real(size);
            freq = fftwf_alloc_complex(bins);
            forward = fftwf_plan_dft_r2c_1d(size, time, freq, FFTW_ESTIMATE);
            backward = fftwf_plan_dft_c2r_1d(size, freq, time, FFTW_ESTIMATE);
            reset();
        }

        void reset() {
            input.assign(size, 0.0f);
            output.assign(size, 0.0f);
            filled = 0;
            readPos = 0;
            smoothed.assign(bins, 0.0f);
            noise.assign(bins, 0.0f);
            minimum.assign(bins, INFINITY);
            previousMinimum.assign(bins, INFINITY);
            lastGainedPower.assign(bins, 0.0f);
            gains.assign(bins, 1.0f);
            strength = 0.0f;
            framesSinceMinimum = 0;
            frames = 0;
            learnt = false;
            previousMinimumValid = false;
            ready.clear();
        }

        void setStrength(float strength) {
            gainFloor.store(std::pow(10.0f, -12.0f * std::clamp<float>(strength, 0.0f, 1.0f) / 20.0f));
        }

        // Output sample i corresponds to input sample i - latency()
        int latency() const { return size - 1; }

        void process(int count, const float* in, float* out) {
            for (int i = 0; i < count; i++) {
                input[filled++] = in[i];
                if (filled == size) {
                    processFrame();
                    // Keep the second half, it is the first half of the next frame
                    std::copy(input.begin() + hop, input.end(), input.begin());
                    filled = size - hop;
                }
                out[i] = (readPos < (int)ready.size()) ? ready[readPos++] : 0.0f;
                if (readPos == (int)ready.size() && readPos > 0) {
                    ready.clear();
                    readPos = 0;
                }
            }
        }

    private:
        void processFrame() {
            float frameFloor = gainFloor.load();
            for (int i = 0; i < size; i++) { time[i] = input[i] * window[i]; }
            fftwf_execute(forward);

            // Silence (gated or muted input) says nothing about the noise
            double framePower = 0.0;
            for (int k = 0; k < bins; k++) { framePower += freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1]; }
            bool silent = framePower < 1e-20;

            // Noise per band: the minimum of the smoothed power over the last two half windows
            // (about 0.75 to 1.5s), so it is found in the pauses of speech and never jumps.
            // The minimum lies under the mean of the noise, the bias corrects for that.
            int halfWindow = std::max<int>(1, (int)(framesPerSecond * MIN_WINDOW_SECONDS / 2.0));
            bool newHalf = !silent && (++framesSinceMinimum >= halfWindow);
            for (int k = 0; k < bins; k++) {
                float p = freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1];
                if (!silent) {
                    smoothed[k] = (frames == 0) ? p : (POWER_SMOOTHING * smoothed[k] + (1.0f - POWER_SMOOTHING) * p);
                    minimum[k] = std::min<float>(minimum[k], smoothed[k]);
                    if (newHalf) {
                        previousMinimum[k] = minimum[k];
                        minimum[k] = smoothed[k];
                    }
                    noise[k] = std::min<float>(minimum[k], previousMinimum[k]) * MINIMUM_BIAS;
                }

                // Nothing is reduced before a whole window was seen, the first frames may be speech
                if (!learnt) { continue; }

                // Decision directed a priori SNR and log-MMSE gain, never below the floor
                float post = (noise[k] > 0.0f) ? p / noise[k] : 1e6f;
                float prior = PRIOR_SMOOTHING * (lastGainedPower[k] / std::max<float>(noise[k], 1e-20f)) + (1.0f - PRIOR_SMOOTHING) * std::max<float>(post - 1.0f, 0.0f);
                prior = std::max<float>(prior, MIN_PRIOR);
                double v = (double)prior * post / (1.0 + prior);
                float gain = (float)std::min(1.0, prior / (1.0 + prior) * std::exp(0.5 * expint(v)));
                gains[k] = std::max<float>(gain, frameFloor);
                lastGainedPower[k] = gains[k] * gains[k] * p;
            }

            if (learnt) {
                // Neighbouring bands with very different gains sound crackly, smooth across them.
                // The reduction fades in once learnt instead of switching on at once.
                strength = std::min<float>(1.0f, strength + (float)(1.0 / (framesPerSecond * FADE_IN_SECONDS)));
                for (int k = 0; k < bins; k++) {
                    int lo = std::max<int>(0, k - SMOOTH_BINS);
                    int hi = std::min<int>(bins - 1, k + SMOOTH_BINS);
                    float g = 0.0f;
                    for (int j = lo; j <= hi; j++) { g += gains[j]; }
                    g /= (float)(hi - lo + 1);
                    g = 1.0f - strength * (1.0f - g);
                    freq[k][0] *= g;
                    freq[k][1] *= g;
                }
            }
            if (newHalf && previousMinimumValid) { learnt = true; }
            if (newHalf) { previousMinimumValid = true; }
            if (newHalf) { framesSinceMinimum = 0; }
            if (!silent) { frames++; }

            fftwf_execute(backward);
            float scale = 1.0f / (float)size;
            for (int i = 0; i < size; i++) { output[i] += time[i] * scale * window[i]; }

            // The first half is complete now
            ready.insert(ready.end(), output.begin(), output.begin() + hop);
            std::copy(output.begin() + hop, output.end(), output.begin());
            std::fill(output.begin() + (size - hop), output.end(), 0.0f);
        }

        // Exponential integral E1 (Abramowitz and Stegun 5.1.53 and 5.1.56), for the log-MMSE gain
        static double expint(double x) {
            if (x <= 0.0) { return 1e10; }
            if (x < 1.0) {
                return -0.57721566 - std::log(x) + x * (0.99999193 + x * (-0.24991055 + x * (0.05519968 + x * (-0.00976004 + x * 0.00107857))));
            }
            return std::exp(-x) / x * (x * x + 2.334733 * x + 0.250621) / (x * x + 3.330657 * x + 1.681534);
        }

        void destroy() {
            if (!time) { return; }
            fftwf_destroy_plan(forward);
            fftwf_destroy_plan(backward);
            fftwf_free(time);
            fftwf_free(freq);
            time = nullptr;
        }

        static constexpr float POWER_SMOOTHING = 0.8f;
        // Smoothing the gain over time keeps weak speech from sounding watery or metallic. Across
        // frequency only the direct neighbours, wider smoothing pulls the harmonics of voiced
        // speech down to the level of the gaps between them (3.6dB lost over 300Hz).
        static constexpr float PRIOR_SMOOTHING = 0.98f;
        // About -25dB: the log-MMSE gain grows again for very small a priori SNRs
        static constexpr float MIN_PRIOR = 0.003f;
        static constexpr int SMOOTH_BINS = 1;
        static constexpr float MINIMUM_BIAS = 1.5f;
        // The gain never goes below the floor: hiss is turned down, not removed, so weak speech
        // keeps its sound. Strength 0..1 maps to a floor of 0dB..-12dB.
        // Set from the UI thread while the DSP thread reads it
        std::atomic<float> gainFloor{ 0.5f };
        static constexpr double MIN_WINDOW_SECONDS = 1.5;
        static constexpr double FADE_IN_SECONDS = 0.5;

        int size = 0;
        int hop = 0;
        int bins = 0;
        double framesPerSecond = 0.0;
        std::vector<float> window;
        float* time = nullptr;
        fftwf_complex* freq = nullptr;
        fftwf_plan forward;
        fftwf_plan backward;

        std::vector<float> input;
        std::vector<float> output;
        std::vector<float> ready;
        int filled = 0;
        int readPos = 0;

        std::vector<float> smoothed;
        std::vector<float> noise;
        std::vector<float> minimum;
        std::vector<float> previousMinimum;
        bool previousMinimumValid = false;
        bool learnt = false;
        std::vector<float> lastGainedPower;
        std::vector<float> gains;
        float strength = 0.0f;
        int framesSinceMinimum = 0;
        long frames = 0;
    };
}
