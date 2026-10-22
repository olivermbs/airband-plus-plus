#pragma once
#include "../processor.h"
#include "speech_clarity.h"

namespace dsp::audio {
    // SpeechClarity as a block of the audio chain
    class SpeechProcessor : public Processor<stereo_t, stereo_t> {
        using base_type = Processor<stereo_t, stereo_t>;
    public:
        SpeechProcessor() {}

        void init(stream<stereo_t>* in, double samplerate) {
            clarity.setSamplerate(samplerate);
            base_type::init(in);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            clarity.setSamplerate(samplerate);
            base_type::tempStart();
        }

        inline int process(int count, const stereo_t* in, stereo_t* out) {
            for (int i = 0; i < count; i++) {
                float l = in[i].l;
                float r = in[i].r;
                clarity.process(l, r);
                out[i] = { l, r };
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
        SpeechClarity clarity;
    };
}
