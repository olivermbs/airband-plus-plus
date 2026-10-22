#pragma once
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>
#include <utils/wav.h>
#include <utils/flog.h>
#include <filesystem>
#include <mutex>
#include <vector>
#include "recording.h"

// Records the radio's audio, one clip per transmission, to a file and in memory for the speech
// recognition. The audio arrives on a DSP thread, the scanner starts and finishes clips from the
// GUI thread.
class TransmissionRecorder {
public:
    struct Finished {
        // Empty when there is no file
        std::string path;
        // Empty when it wasn't kept
        std::vector<float> audio;
        float sampleRate = 0.0f;
    };

    // Shorter is a squelch tail or a click, not something worth listening back to
    static constexpr double MIN_AUDIBLE_SECONDS = 0.5;

    TransmissionRecorder() {
        sink.init(NULL, handler, this);
        unregisterHandler.handler = streamUnregisterHandler;
        unregisterHandler.ctx = this;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&unregisterHandler);
    }

    ~TransmissionRecorder() {
        detach();
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&unregisterHandler);
    }

    // Listens to an audio stream, the radio's is named after its VFO
    void attach(const std::string& streamName) {
        if (input && streamName == attachedName) { return; }
        detach();
        input = sigpath::sinkManager.bindStream(streamName);
        if (!input) { return; }
        attachedName = streamName;
        sink.setInput(input);
        sink.start();
    }

    // A clip being recorded is kept if asked to and it holds enough sound
    void detach(bool keepClip = false) {
        if (!input) { return; }
        finish(keepClip);
        // Stopped before unbinding, the stream is deleted when unbound
        sink.stop();
        sigpath::sinkManager.unbindStream(attachedName, input);
        input = NULL;
        attachedName.clear();
    }

    bool isAttached() { return input != NULL; }

    bool isRecording() {
        std::lock_guard<std::mutex> lck(mtx);
        return active;
    }

    // Starts a clip, written to the path or, with an empty one, only kept in memory
    bool start(const std::string& path) {
        if (!input) { return false; }
        finish(false);

        float sampleRate = sigpath::sinkManager.getStreamSampleRate(attachedName);
        if (sampleRate <= 0.0f) { return false; }
        std::error_code ec;
        if (!path.empty()) { std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec); }

        std::lock_guard<std::mutex> lck(mtx);
        if (!path.empty()) {
            writer.setChannels(1);
            writer.setSamplerate((uint64_t)sampleRate);
            writer.setFormat(wav::FORMAT_WAV);
            writer.setSampleType(wav::SAMP_TYPE_INT16);
            if (!writer.open(path)) {
                flog::error("Scanner: could not create recording {0}", path);
                return false;
            }
        }
        active = true;
        clipPath = path;
        clipRate = sampleRate;
        audio.clear();
        trimmer.reset((int)(MAX_PAUSE_SECONDS * sampleRate));
        return true;
    }

    // Closes the clip. It is kept when asked to and it holds enough sound, else its file is
    // deleted and its audio dropped.
    Finished finish(bool keep) {
        std::lock_guard<std::mutex> lck(mtx);
        Finished finished;
        if (!active) { return finished; }

        active = false;
        if (!clipPath.empty()) { writer.close(); }
        std::string path = clipPath;
        clipPath.clear();
        double audible = (double)trimmer.audibleSamples() / clipRate;
        if (keep && audible >= MIN_AUDIBLE_SECONDS) {
            finished.path = path;
            finished.audio = std::move(audio);
            finished.sampleRate = clipRate;
            audio.clear();
            return finished;
        }

        audio.clear();
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        return finished;
    }

private:
    static void handler(dsp::stereo_t* data, int count, void* ctx) {
        TransmissionRecorder* _this = (TransmissionRecorder*)ctx;
        std::lock_guard<std::mutex> lck(_this->mtx);
        if (!_this->active) { return; }

        float* mono = _this->monoBuf.data();
        for (int i = 0; i < count; i++) {
            mono[i] = (data[i].l + data[i].r) * 0.5f;
        }
        int kept = _this->trimmer.keep(mono, count);
        if (kept <= 0) { return; }
        if (!_this->clipPath.empty()) { _this->writer.write(mono, kept); }
        if (_this->audio.size() < (size_t)(MAX_AUDIO_SECONDS * _this->clipRate)) {
            _this->audio.insert(_this->audio.end(), mono, mono + kept);
        }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        TransmissionRecorder* _this = (TransmissionRecorder*)ctx;
        // The radio went away, what it already said is kept
        if (name == _this->attachedName) { _this->detach(true); }
    }

    // Long enough to tell one over from the next
    static constexpr double MAX_PAUSE_SECONDS = 0.7;
    // Kept in memory for the speech recognition, a reception longer than this is cut there
    static constexpr double MAX_AUDIO_SECONDS = 120.0;

    dsp::stream<dsp::stereo_t>* input = NULL;
    std::string attachedName;
    dsp::sink::Handler<dsp::stereo_t> sink;
    EventHandler<std::string> unregisterHandler;

    std::mutex mtx;
    wav::Writer writer;
    bool active = false;
    std::string clipPath;
    std::vector<float> audio;
    float clipRate = 48000.0f;
    scan::SilenceTrimmer trimmer;
    std::vector<float> monoBuf = std::vector<float>(STREAM_BUFFER_SIZE);
};
