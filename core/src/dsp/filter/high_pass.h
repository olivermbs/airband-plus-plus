#pragma once
#include "../processor.h"
#include "butterworth.h"

namespace dsp::filter {
    // Audio high-pass, a 6th order Butterworth IIR
    class HighPass : public Processor<stereo_t, stereo_t> {
        using base_type = Processor<stereo_t, stereo_t>;
    public:
        HighPass() {}

        HighPass(stream<stereo_t>* in, double cutoff, double samplerate) { init(in, cutoff, samplerate); }

        void init(stream<stereo_t>* in, double cutoff, double samplerate) {
            _cutoff = cutoff;
            filter.design(_cutoff, samplerate);
            base_type::init(in);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            filter.design(_cutoff, samplerate);
            base_type::tempStart();
        }

        inline int process(int count, const stereo_t* in, stereo_t* out) {
            for (int i = 0; i < count; i++) {
                out[i].l = filter.process(in[i].l, 0);
                out[i].r = filter.process(in[i].r, 1);
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
        double _cutoff;
        ButterworthHighPass filter;
    };
}
