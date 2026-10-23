#pragma once
#include <algorithm>
#include <cmath>

namespace dsp::audio {
    // Makes radio voice easier to understand, after the squelch gate:
    // - Presence EQ: voice energy falls about 25dB from 500Hz to 2.5kHz, so the consonants that
    //   tell callsigns apart drown under the vowels. They are lifted 6dB around 2.2kHz, and the
    //   hiss above the voice is cut off steeply.
    // - Leveler: with the carrier AGC loudness follows how hard each transmitter modulates, a
    //   weak aircraft and a loud tower come out several dB apart. The gain only adapts while
    //   there is sound, the squelch gate's silence would otherwise pull it up between overs.
    // - Limiter: peaks would otherwise clip at the sound card.
    // Chosen by ear on airband recordings, with 50% noise reduction before it. Speech comes out
    // around -12dBFS, at least as loud as the radio without it, the limiter touching about 0.1%
    // of the samples.
    class SpeechClarity {
    public:
        static constexpr float CEILING = 0.891250938f; // -1dBFS
        // Level the envelope is brought to, it follows syllable peaks so speech averages lower
        static constexpr double TARGET_DB = -6.0;

        void setSamplerate(double samplerate) {
            const double PRESENCE_HZ = 2200.0;
            const double PRESENCE_DB = 6.0;
            const double PRESENCE_Q = 0.8;
            const double CUTOFF_HZ = 3300.0;
            // Q of the two biquads of a 4th order Butterworth
            const double LOWPASS_Q[2] = { 0.54119610, 1.30656296 };

            presence = Biquad::peaking(PRESENCE_HZ, samplerate, PRESENCE_DB, PRESENCE_Q);
            lowpass[0] = Biquad::lowpass(CUTOFF_HZ, samplerate, LOWPASS_Q[0]);
            lowpass[1] = Biquad::lowpass(CUTOFF_HZ, samplerate, LOWPASS_Q[1]);

            envAttack = 1.0 - std::exp(-1.0 / (ENV_ATTACK_S * samplerate));
            envRelease = 1.0 - std::exp(-1.0 / (ENV_RELEASE_S * samplerate));
            gainSmoothing = 1.0 - std::exp(-1.0 / (GAIN_SMOOTHING_S * samplerate));
            limiterRelease = 1.0 - std::exp(-1.0 / (LIMITER_RELEASE_S * samplerate));
            reset();
        }

        void reset() {
            presence.clear();
            lowpass[0].clear();
            lowpass[1].clear();
            envelope = 0.0;
            gain = 1.0;
            limiterGain = 1.0;
        }

        void setLeveling(bool enabled) { leveling = enabled; }

        inline void process(float& l, float& r) {
            bool sound = std::fabs(l) > SILENCE || std::fabs(r) > SILENCE;
            double left = equalize(l, 0);
            double right = equalize(r, 1);

            if (leveling) {
                double mid = 0.5 * (left + right);
                double power = mid * mid;
                if (sound) { envelope += ((power > envelope) ? envAttack : envRelease) * (power - envelope); }
                if (envelope > 1e-12) {
                    double wanted = std::clamp<double>(TARGET / std::sqrt(envelope), MIN_GAIN, MAX_GAIN);
                    gain += gainSmoothing * (wanted - gain);
                }
                left *= gain;
                right *= gain;
            }

            // Instant attack, a peak above the ceiling sets the gain that brings it down to it.
            // Released before the check, the release must not push the sample over.
            double peak = std::max<double>(std::fabs(left), std::fabs(right));
            limiterGain += limiterRelease * (1.0 - limiterGain);
            if (peak * limiterGain > CEILING) { limiterGain = CEILING / peak; }
            l = (float)(left * limiterGain);
            r = (float)(right * limiterGain);
        }

    private:
        struct Biquad {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
            double z1[2] = {}, z2[2] = {};

            static Biquad normalized(double b0, double b1, double b2, double a0, double a1, double a2) {
                Biquad q;
                q.b0 = b0 / a0;
                q.b1 = b1 / a0;
                q.b2 = b2 / a0;
                q.a1 = a1 / a0;
                q.a2 = a2 / a0;
                return q;
            }

            static Biquad lowpass(double freq, double samplerate, double q) {
                double w0 = 2.0 * M_PI * freq / samplerate;
                double cosW0 = std::cos(w0);
                double alpha = std::sin(w0) / (2.0 * q);
                return normalized((1.0 - cosW0) / 2.0, 1.0 - cosW0, (1.0 - cosW0) / 2.0, 1.0 + alpha, -2.0 * cosW0, 1.0 - alpha);
            }

            static Biquad peaking(double freq, double samplerate, double gainDb, double q) {
                double a = std::pow(10.0, gainDb / 40.0);
                double w0 = 2.0 * M_PI * freq / samplerate;
                double cosW0 = std::cos(w0);
                double alpha = std::sin(w0) / (2.0 * q);
                return normalized(1.0 + alpha * a, -2.0 * cosW0, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cosW0, 1.0 - alpha / a);
            }

            void clear() {
                z1[0] = z1[1] = 0.0;
                z2[0] = z2[1] = 0.0;
            }

            // Transposed direct form II
            inline double process(double x, int channel) {
                double y = b0 * x + z1[channel];
                z1[channel] = b1 * x - a1 * y + z2[channel];
                z2[channel] = b2 * x - a2 * y;
                return y;
            }
        };

        inline double equalize(double x, int channel) {
            x = presence.process(x, channel);
            x = lowpass[0].process(x, channel);
            return lowpass[1].process(x, channel);
        }

        // The squelch gate outputs exact zeros while closed, its fades are well above this
        static constexpr float SILENCE = 1e-5f;
        static constexpr double TARGET = 0.501187234; // TARGET_DB as amplitude
        // Carrier AGC puts a fully modulated transmitter near full scale
        static constexpr double MIN_GAIN = 0.063095734; // -24dB
        // +25dB: a weak transmission comes out as loud as the others, more would only raise the
        // noise between its words
        static constexpr double MAX_GAIN = 17.782794100;
        static constexpr double ENV_ATTACK_S = 0.010;
        static constexpr double ENV_RELEASE_S = 0.400;
        static constexpr double GAIN_SMOOTHING_S = 0.050;
        static constexpr double LIMITER_RELEASE_S = 0.060;

        Biquad presence;
        Biquad lowpass[2];
        bool leveling = true;
        double envAttack = 0.0;
        double envRelease = 0.0;
        double gainSmoothing = 0.0;
        double limiterRelease = 0.0;
        double envelope = 0.0;
        double gain = 1.0;
        double limiterGain = 1.0;
    };
}
