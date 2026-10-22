#pragma once
#include "../processor.h"
#include "noise_floor_tracker.h"
#include <atomic>

// TODO: Rewrite better!!!!!
namespace dsp::noise_reduction {
    class PowerSquelch : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        PowerSquelch() {}

        PowerSquelch(stream<complex_t>* in, double level) {}

        ~PowerSquelch() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            buffer::free(normBuffer);
        }

        void init(stream<complex_t>* in, double level) {
            _level = level;

            normBuffer = buffer::alloc<float>(STREAM_BUFFER_SIZE);

            base_type::init(in);
        }

        void setLevel(double level) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _level = level;
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::mutex> lck(trackerMtx);
            _samplerate = samplerate;
        }

        // Automatic mode: the threshold follows the noise floor instead of the fixed level
        void setAuto(bool enabled) {
            assert(base_type::_block_init);
            std::lock_guard<std::mutex> lck(trackerMtx);
            if (enabled && !_auto) { tracker.reset(); }
            _auto = enabled;
        }

        void setAutoMargin(double margin) {
            assert(base_type::_block_init);
            std::lock_guard<std::mutex> lck(trackerMtx);
            tracker.setMargin(margin);
        }

        // The frequency changed, see NoiseFloorTracker::retune()
        void retune() {
            std::lock_guard<std::mutex> lck(trackerMtx);
            tracker.retune();
        }

        void setReferenceFloor(float floor) {
            std::lock_guard<std::mutex> lck(trackerMtx);
            tracker.setReferenceFloor(floor);
        }

        void clearReferenceFloor() {
            std::lock_guard<std::mutex> lck(trackerMtx);
            tracker.clearReferenceFloor();
        }

        // Keeps the output silent while still measuring, e.g. while a scanner is searching
        void setForceClosed(bool forceClosed) { _forceClosed = forceClosed; }

        // Level and noise floor of the auto mode, in dB
        float getLevel() { return _lastLevel; }
        float getNoiseFloor() { return _noiseFloor; }
        bool isOpen() { return _open; }

        inline int process(int count, const complex_t* in, complex_t* out) {
            // Compute the amplitude of each sample
            volk_32fc_magnitude_32f(normBuffer, (lv_32fc_t*)in, count);
            
            // Compute the mean amplitude
            float sum = 0.0f;
            volk_32f_accumulator_s32f(&sum, normBuffer, count);
            sum /= (float)count;

            // The manual level keeps its historical scale, auto mode works in real dB
            float level = 10.0f * log10f(sum);
            float levelDb = 2.0f * level;
            bool open;
            std::unique_lock<std::mutex> lck(trackerMtx);
            if (_auto) {
                open = tracker.update(levelDb, (double)count / _samplerate);
                _noiseFloor = tracker.getNoiseFloor();
            }
            else {
                open = level >= _level;
            }
            lck.unlock();
            _lastLevel = levelDb;
            _open = open;

            if (open && !_forceClosed) {
                memcpy(out, in, count * sizeof(complex_t));
            }
            else {
                memset(out, 0, count * sizeof(complex_t));
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
        float* normBuffer;
        float _level = -50.0f;
        double _samplerate = 50000.0;
        bool _auto = false;
        std::mutex trackerMtx;
        NoiseFloorTracker tracker;
        std::atomic<float> _lastLevel = -INFINITY;
        std::atomic<float> _noiseFloor = -INFINITY;
        std::atomic<bool> _open = false;
        std::atomic<bool> _forceClosed = false;
                
    };
}