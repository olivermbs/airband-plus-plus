#pragma once
#include <cmath>

namespace dsp::filter {
    // 6th order Butterworth high-pass as three biquads, for up to two channels. An IIR filter
    // needs a handful of operations per sample where a FIR with a similar response needs
    // thousands of taps.
    class ButterworthHighPass {
    public:
        void design(double cutoff, double samplerate) {
            // Q of each biquad of a 6th order Butterworth
            const double Q[STAGES] = { 0.51763809, 0.70710678, 1.93185165 };
            double w0 = 2.0 * M_PI * cutoff / samplerate;
            double cosW0 = std::cos(w0);
            for (int s = 0; s < STAGES; s++) {
                double alpha = std::sin(w0) / (2.0 * Q[s]);
                double a0 = 1.0 + alpha;
                stages[s].b0 = ((1.0 + cosW0) / 2.0) / a0;
                stages[s].b1 = -(1.0 + cosW0) / a0;
                stages[s].b2 = stages[s].b0;
                stages[s].a1 = (-2.0 * cosW0) / a0;
                stages[s].a2 = (1.0 - alpha) / a0;
            }
            reset();
        }

        void reset() {
            for (auto& stage : stages) {
                for (int c = 0; c < CHANNELS; c++) {
                    stage.z1[c] = 0.0f;
                    stage.z2[c] = 0.0f;
                }
            }
        }

        inline float process(float x, int channel) {
            for (auto& stage : stages) {
                // Transposed direct form II
                float y = stage.b0 * x + stage.z1[channel];
                stage.z1[channel] = stage.b1 * x - stage.a1 * y + stage.z2[channel];
                stage.z2[channel] = stage.b2 * x - stage.a2 * y;
                x = y;
            }
            return x;
        }

    private:
        static constexpr int STAGES = 3;
        static constexpr int CHANNELS = 2;

        struct Stage {
            float b0, b1, b2, a1, a2;
            float z1[CHANNELS];
            float z2[CHANNELS];
        };
        Stage stages[STAGES] = {};
    };
}
