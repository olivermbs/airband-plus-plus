#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

// The parts of recording transmissions that don't touch the audio system or files, so they can
// be tested on their own
namespace scan {
    // The radio's squelch gate outputs exact zeros while closed, its fades are well above this
    constexpr float SILENCE_LEVEL = 1e-5f;

    inline bool isSilent(const float* samples, int count) {
        for (int i = 0; i < count; i++) {
            if (std::fabs(samples[i]) >= SILENCE_LEVEL) { return false; }
        }
        return true;
    }

    // Decides what of a transmission's audio is written: the silence before the first sound is
    // dropped and pauses between overs are shortened, so a clip is the conversation and little else
    class SilenceTrimmer {
    public:
        void reset(int maxPauseSamples) {
            maxPause = std::max<int>(maxPauseSamples, 0);
            pause = 0;
            audible = 0;
            started = false;
        }

        // How many samples from the start of the block to write
        int keep(const float* samples, int count) {
            if (!isSilent(samples, count)) {
                started = true;
                pause = 0;
                audible += count;
                return count;
            }
            if (!started) { return 0; }

            int kept = std::min<int>(count, maxPause - pause);
            pause += kept;
            return kept;
        }

        int64_t audibleSamples() const { return audible; }

    private:
        int maxPause = 0;
        int pause = 0;
        int64_t audible = 0;
        bool started = false;
    };

    // Recorded frequencies are kept in Hz, a channel matches one within half a channel step
    inline bool matchesAny(const std::set<long long>& freqs, double freq, double step) {
        for (long long f : freqs) {
            if (std::fabs((double)f - freq) < step / 2.0) { return true; }
        }
        return false;
    }

    // A path that doesn't exist yet: the name, then the name with _2, _3 and so on. A file
    // opened for recording is truncated, a second clip in the same second would replace the first.
    inline std::string uniquePath(const std::string& base, const std::string& extension, const std::function<bool(const std::string&)>& exists) {
        std::string path = base + extension;
        for (int i = 2; exists(path); i++) {
            path = base + "_" + std::to_string(i) + extension;
        }
        return path;
    }

    // A bookmark name made safe for a file name
    inline std::string fileNamePart(const std::string& name) {
        std::string part;
        for (char c : name) {
            bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
            part += safe ? c : '-';
        }
        return part;
    }

    struct Clip {
        uint32_t sampleRate = 0;
        std::vector<float> samples;
    };

    // Reads 16 bit PCM, which is what the scanner records, stereo is mixed down to mono. A file
    // that wasn't closed properly has no data size, the rest of the file is taken then.
    inline bool parseWav(const std::vector<uint8_t>& data, Clip& clip) {
        auto u16 = [&](size_t at) { return (uint16_t)(data[at] | (data[at + 1] << 8)); };
        auto u32 = [&](size_t at) { return (uint32_t)u16(at) | ((uint32_t)u16(at + 2) << 16); };

        if (data.size() < 12 || memcmp(&data[0], "RIFF", 4) != 0 || memcmp(&data[8], "WAVE", 4) != 0) { return false; }

        uint16_t channels = 0;
        uint16_t bits = 0;
        size_t pos = 12;
        while (pos + 8 <= data.size()) {
            uint32_t size = u32(pos + 4);
            size_t body = pos + 8;

            if (memcmp(&data[pos], "fmt ", 4) == 0) {
                if (size < 16 || body + 16 > data.size()) { return false; }
                if (u16(body) != 1) { return false; }
                channels = u16(body + 2);
                clip.sampleRate = u32(body + 4);
                bits = u16(body + 14);
            }

            if (memcmp(&data[pos], "data", 4) == 0) {
                if (channels == 0 || bits != 16 || clip.sampleRate == 0) { return false; }
                size_t available = data.size() - body;
                size_t bytes = (size == 0 || size > available) ? available : size;
                size_t frames = bytes / (2 * channels);
                clip.samples.resize(frames);
                for (size_t i = 0; i < frames; i++) {
                    float sum = 0.0f;
                    for (int c = 0; c < channels; c++) {
                        sum += (float)(int16_t)u16(body + (i * channels + c) * 2) / 32768.0f;
                    }
                    clip.samples[i] = sum / (float)channels;
                }
                return true;
            }

            // Chunks are padded to an even size
            pos = body + size + (size & 1);
        }
        return false;
    }
}
