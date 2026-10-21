#pragma once
#include <algorithm>
#include <cmath>

namespace dsp::noise_reduction {
    // Tracks the noise floor of a level measurement (in dB) and opens a gate when
    // the level rises a given margin above it. Used for automatic squelch.
    //
    // The floor follows the noise between transmissions as a low quantile: it falls
    // quickly and rises slowly, so it settles near the bottom of the noise fluctuation.
    // While the gate is open the floor barely rises, so a long transmission is not
    // mistaken for noise, but a permanent rise of the noise level is still recovered from.
    class NoiseFloorTracker {
    public:
        void setMargin(float margin) { _margin = margin; }
        void setHysteresis(float hysteresis) { _hysteresis = hysteresis; }
        void setHangTime(double seconds) { _hangTime = seconds; }

        void reset() {
            _initialized = false;
            _open = false;
        }

        bool update(float level, double duration) {
            if (!std::isfinite(level)) { return _open; }
            _level = level;

            if (!_initialized) {
                _floor = level;
                _initialized = true;
            }

            float threshold = _floor + _margin;
            if (level >= threshold) {
                _open = true;
                _hangRemaining = _hangTime;
            }
            else if (_open && level < threshold - _hysteresis) {
                _hangRemaining -= duration;
                if (_hangRemaining <= 0.0) { _open = false; }
            }

            if (level < _floor) {
                _floor = std::max<float>(level, _floor - FALL_RATE * duration);
                return _open;
            }

            float riseRate = _open ? OPEN_RISE_RATE : RISE_RATE;
            _floor = std::min<float>(level, _floor + riseRate * duration);
            return _open;
        }

        bool isOpen() const { return _open; }
        float getNoiseFloor() const { return _floor; }
        float getLevel() const { return _level; }
        float getThreshold() const { return _floor + _margin; }

    private:
        // dB per second. The fall/rise ratio puts the floor near the 20th percentile of the noise.
        static constexpr float FALL_RATE = 10.0f;
        static constexpr float RISE_RATE = 2.5f;
        static constexpr float OPEN_RISE_RATE = 0.05f;

        float _margin = 3.0f;
        float _hysteresis = 1.0f;
        double _hangTime = 0.3;

        bool _initialized = false;
        bool _open = false;
        float _floor = 0.0f;
        float _level = 0.0f;
        double _hangRemaining = 0.0;
    };
}
