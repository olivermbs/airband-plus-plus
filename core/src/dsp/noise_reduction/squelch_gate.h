#pragma once
#include "../processor.h"
#include <atomic>
#include <cmath>

namespace dsp::noise_reduction {
    // Mutes audio while a squelch is closed, with short raised-cosine fades. Gating the audio
    // instead of the IF keeps the demodulator, its AGC and DC blocker running on a continuous
    // signal, cutting the IF to zeros made them thump and chirp at every opening and closing.
    // Opening waits a moment: a carrier appearing makes the demodulator's DC blocker settle for
    // about 10ms, which would be heard as a thump. Transmitters key the carrier before speech.
    class SquelchGate : public Processor<stereo_t, stereo_t> {
        using base_type = Processor<stereo_t, stereo_t>;
    public:
        SquelchGate() {}

        void init(stream<stereo_t>* in, const std::atomic<bool>* open, double samplerate) {
            _open = open;
            setSamplerateUnlocked(samplerate);
            base_type::init(in);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            setSamplerateUnlocked(samplerate);
        }

        inline int process(int count, const stereo_t* in, stereo_t* out) {
            bool open = _open->load();
            for (int i = 0; i < count; i++) {
                openFor = open ? openFor + 1 : 0;
                float target = (openFor > openDelay) ? 1.0f : 0.0f;
                if (position < target) { position = std::min<float>(target, position + openStep); }
                else if (position > target) { position = std::max<float>(target, position - closeStep); }
                float gain = 0.5f - 0.5f * std::cos((float)M_PI * position);
                out[i] = { in[i].l * gain, in[i].r * gain };
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        void setSamplerateUnlocked(double samplerate) {
            openStep = 1.0f / (float)(OPEN_SECONDS * samplerate);
            openDelay = (int)(OPEN_DELAY_SECONDS * samplerate);
            closeStep = 1.0f / (float)(CLOSE_SECONDS * samplerate);
        }

        static constexpr double OPEN_SECONDS = 0.008;
        static constexpr double OPEN_DELAY_SECONDS = 0.02;
        static constexpr double CLOSE_SECONDS = 0.025;

        const std::atomic<bool>* _open = nullptr;
        float position = 0.0f;
        float openStep = 0.0f;
        float closeStep = 0.0f;
        int openDelay = 0;
        int openFor = 0;
    };
}
