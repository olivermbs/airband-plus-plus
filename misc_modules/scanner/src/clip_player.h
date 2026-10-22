#pragma once
#include <core.h>
#include <signal_path/signal_path.h>
#include <dsp/multirate/rational_resampler.h>
#include <utils/flog.h>
#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>
#include "recording.h"

// Plays recorded transmissions on an audio stream of its own, so it has its own output and
// volume in the sinks menu and the live radio keeps playing
class ClipPlayer {
public:
    static constexpr const char* STREAM_NAME = "Scanner Playback";

    ClipPlayer() {
        resampler.init(&input, 48000.0, 48000.0);
        srChangeHandler.handler = sampleRateChangeHandler;
        srChangeHandler.ctx = this;
        stream.init(&resampler.out, &srChangeHandler, 48000.0f);
        sigpath::sinkManager.registerStream(STREAM_NAME, &stream);
    }

    ~ClipPlayer() {
        stop();
        sigpath::sinkManager.unregisterStream(STREAM_NAME);
    }

    // A new stream starts without a sink, the audio sink is what playback is for
    void selectDefaultSink() {
        core::configManager.acquire();
        bool configured = core::configManager.conf["streams"].contains(STREAM_NAME);
        core::configManager.release();
        if (configured) { return; }
        sigpath::sinkManager.setStreamSink(STREAM_NAME, "Audio");
    }

    bool play(const std::string& path) {
        stop();

        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        clip = scan::Clip();
        if (!scan::parseWav(data, clip)) {
            flog::error("Scanner: could not read recording {0}", path);
            return false;
        }

        // The end is still in the resampler and the sink's buffer when the last block is handed
        // over, silence pushes it through before the stream stops
        clip.samples.resize(clip.samples.size() + (size_t)(TAIL_SECONDS * clip.sampleRate), 0.0f);

        resampler.setInSamplerate(clip.sampleRate);
        resampler.start();
        stream.start();
        finished = false;
        playingPath = path;
        worker = std::thread(&ClipPlayer::run, this);
        return true;
    }

    void stop() {
        if (playingPath.empty()) { return; }
        input.stopWriter();
        if (worker.joinable()) { worker.join(); }
        input.clearWriteStop();
        stream.stop();
        resampler.stop();
        playingPath.clear();
    }

    // Called every frame, releases the audio device once a clip has played
    void poll() {
        if (!playingPath.empty() && finished) { stop(); }
    }

    const std::string& playing() { return playingPath; }

private:
    void run() {
        size_t block = std::max<size_t>(clip.sampleRate / BLOCKS_PER_SECOND, 1);
        for (size_t pos = 0; pos < clip.samples.size(); pos += block) {
            size_t count = std::min<size_t>(block, clip.samples.size() - pos);
            for (size_t i = 0; i < count; i++) {
                float s = clip.samples[pos + i];
                input.writeBuf[i] = { s, s };
            }
            // Blocks until the sink took the previous block, which paces playback
            if (!input.swap((int)count)) { break; }
        }
        finished = true;
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        ClipPlayer* _this = (ClipPlayer*)ctx;
        _this->resampler.setOutSamplerate(sampleRate);
    }

    static constexpr size_t BLOCKS_PER_SECOND = 50;
    static constexpr double TAIL_SECONDS = 0.3;

    dsp::stream<dsp::stereo_t> input;
    dsp::multirate::RationalResampler<dsp::stereo_t> resampler;
    SinkManager::Stream stream;
    EventHandler<float> srChangeHandler;

    scan::Clip clip;
    std::thread worker;
    std::atomic<bool> finished = false;
    std::string playingPath;
};
