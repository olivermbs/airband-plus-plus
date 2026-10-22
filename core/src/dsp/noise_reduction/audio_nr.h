#pragma once
#include "../processor.h"
#include "spectral_nr.h"

namespace dsp::noise_reduction {
    // Spectral noise reduction of an audio stream, each channel on its own
    class AudioNoiseReduction : public Processor<stereo_t, stereo_t> {
        using base_type = Processor<stereo_t, stereo_t>;
    public:
        AudioNoiseReduction() {}

        void init(stream<stereo_t>* in, double samplerate) {
            left.init(samplerate);
            right.init(samplerate);
            base_type::init(in);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            left.init(samplerate);
            right.init(samplerate);
            base_type::tempStart();
        }

        inline int process(int count, const stereo_t* in, stereo_t* out) {
            if ((int)leftIn.size() < count) {
                leftIn.resize(count);
                rightIn.resize(count);
                leftOut.resize(count);
                rightOut.resize(count);
            }
            for (int i = 0; i < count; i++) {
                leftIn[i] = in[i].l;
                rightIn[i] = in[i].r;
            }
            left.process(count, leftIn.data(), leftOut.data());
            right.process(count, rightIn.data(), rightOut.data());
            for (int i = 0; i < count; i++) {
                out[i].l = leftOut[i];
                out[i].r = rightOut[i];
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
        SpectralNoiseReducer left;
        SpectralNoiseReducer right;
        std::vector<float> leftIn, rightIn, leftOut, rightOut;
    };
}
