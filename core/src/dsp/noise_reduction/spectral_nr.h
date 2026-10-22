#pragma once
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp::noise_reduction {
    // Audio noise reduction for one channel. Each FFT band is turned down according to its own
    // signal to noise ratio. The noise of each band is its smoothed power at the quietest moments,
    // so it is learnt during the pauses of speech. The gain is smoothed over time (decision
    // directed) and never goes below a floor, which avoids the warbling "musical noise".
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
            lastGainedPower.assign(bins, 0.0f);
            framesSinceMinimum = 0;
            frames = 0;
            ready.clear();
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
            for (int i = 0; i < size; i++) { time[i] = input[i] * window[i]; }
            fftwf_execute(forward);

            // Noise per band: the minimum of the smoothed power over a sliding window of about
            // 1.5s, approximated with two halves, corrected for the minimum being under the mean
            int windowFrames = std::max<int>(1, (int)(framesPerSecond * MIN_WINDOW_SECONDS / 2.0));
            bool newHalf = (++framesSinceMinimum >= windowFrames);
            for (int k = 0; k < bins; k++) {
                float p = freq[k][0] * freq[k][0] + freq[k][1] * freq[k][1];
                smoothed[k] = (frames == 0) ? p : (POWER_SMOOTHING * smoothed[k] + (1.0f - POWER_SMOOTHING) * p);
                minimum[k] = std::min<float>(minimum[k], smoothed[k]);
                if (frames == 0) { noise[k] = smoothed[k]; }
                noise[k] = std::min<float>(noise[k], minimum[k] * MINIMUM_BIAS);
                if (newHalf) {
                    noise[k] = minimum[k] * MINIMUM_BIAS;
                    minimum[k] = smoothed[k];
                }

                // Decision directed a priori SNR and Wiener gain
                float post = (noise[k] > 0.0f) ? p / noise[k] : 1e6f;
                float prior = PRIOR_SMOOTHING * (lastGainedPower[k] / std::max<float>(noise[k], 1e-20f)) + (1.0f - PRIOR_SMOOTHING) * std::max<float>(post - 1.0f, 0.0f);
                float gain = std::max<float>(prior / (1.0f + prior), GAIN_FLOOR);
                lastGainedPower[k] = gain * gain * p;
                freq[k][0] *= gain;
                freq[k][1] *= gain;
            }
            if (newHalf) { framesSinceMinimum = 0; }
            frames++;

            fftwf_execute(backward);
            float scale = 1.0f / (float)size;
            for (int i = 0; i < size; i++) { output[i] += time[i] * scale * window[i]; }

            // The first half is complete now
            ready.insert(ready.end(), output.begin(), output.begin() + hop);
            std::copy(output.begin() + hop, output.end(), output.begin());
            std::fill(output.begin() + (size - hop), output.end(), 0.0f);
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
        static constexpr float PRIOR_SMOOTHING = 0.98f;
        static constexpr float MINIMUM_BIAS = 1.6f;
        static constexpr float GAIN_FLOOR = 0.1f;
        static constexpr double MIN_WINDOW_SECONDS = 1.5;

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
        std::vector<float> lastGainedPower;
        int framesSinceMinimum = 0;
        long frames = 0;
    };
}
