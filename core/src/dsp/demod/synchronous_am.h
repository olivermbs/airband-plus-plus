#pragma once
#include "../types.h"
#include "../loop/phase_control_loop.h"
#include <cmath>

namespace dsp::demod {
    // Synchronous AM detection: a PLL locks onto the carrier, the signal is rotated so the
    // carrier sits at zero phase and only the in-phase part is kept. That discards the
    // quadrature half of the noise, and unlike an envelope detector it has no threshold below
    // which noise swamps a weak signal. A frequency-locked loop pulls the PLL onto carriers
    // that are well off frequency, as aircraft often are.
    class SynchronousDetector {
    public:
        // A loop narrower than about 150Hz wanders and slips on weak signals, measured on
        // simulated AM at 0-10dB carrier to noise
        void init(double samplerate, double loopBandwidth = 150.0, double maxOffset = 3000.0) {
            float alpha, beta;
            loop::PhaseControlLoop<float>::criticallyDamped(loopBandwidth / samplerate, alpha, beta);
            float maxFreq = 2.0 * M_PI * maxOffset / samplerate;
            pcl.init(alpha, beta, 0.0f, -M_PI, M_PI, 0.0f, -maxFreq, maxFreq);
            reset();
        }

        void reset() {
            pcl.phase = 0.0f;
            pcl.freq = 0.0f;
            last = { 0.0f, 0.0f };
            freqError = { 0.0f, 0.0f };
        }

        void process(int count, const complex_t* in, float* out) {
            for (int i = 0; i < count; i++) {
                // Rotate by the opposite of the tracked carrier phase
                float c = std::cos(pcl.phase);
                float s = std::sin(pcl.phase);
                complex_t y = { (in[i].re * c) + (in[i].im * s), (in[i].im * c) - (in[i].re * s) };
                out[i] = y.re;

                // Remaining rotation from one sample to the next is the frequency error. Averaged
                // as a complex sum it is dominated by the coherent carrier rather than the noise.
                complex_t step = { (y.re * last.re) + (y.im * last.im), (y.im * last.re) - (y.re * last.im) };
                freqError.re += FLL_SMOOTHING * (step.re - freqError.re);
                freqError.im += FLL_SMOOTHING * (step.im - freqError.im);
                last = y;
                pcl.freq += FLL_GAIN * std::atan2(freqError.im, freqError.re);

                pcl.advance(std::atan2(y.im, y.re));
            }
        }

        // Tracked carrier offset in radians per sample
        float frequency() const { return pcl.freq; }

    private:
        static constexpr float FLL_SMOOTHING = 0.02f;
        static constexpr float FLL_GAIN = 0.0005f;

        loop::PhaseControlLoop<float> pcl;
        complex_t last;
        complex_t freqError;
    };
}
