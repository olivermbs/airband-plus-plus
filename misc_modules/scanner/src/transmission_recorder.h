#pragma once
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>
#include <utils/wav.h>
#include <utils/flog.h>
#include <filesystem>
#include <mutex>
#include "recording.h"

// Records the radio's audio, one file per transmission. The audio arrives on a DSP thread, the
// scanner starts and finishes clips from the GUI thread.
class TransmissionRecorder {
public:
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
        return !clipPath.empty();
    }

    bool start(const std::string& path) {
        if (!input) { return false; }
        finish(false);

        float sampleRate = sigpath::sinkManager.getStreamSampleRate(attachedName);
        if (sampleRate <= 0.0f) { return false; }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        std::lock_guard<std::mutex> lck(mtx);
        writer.setChannels(1);
        writer.setSamplerate((uint64_t)sampleRate);
        writer.setFormat(wav::FORMAT_WAV);
        writer.setSampleType(wav::SAMP_TYPE_INT16);
        if (!writer.open(path)) {
            flog::error("Scanner: could not create recording {0}", path);
            return false;
        }
        clipPath = path;
        clipRate = sampleRate;
        trimmer.reset((int)(MAX_PAUSE_SECONDS * sampleRate));
        return true;
    }

    // Closes the clip. It is kept when asked to and it holds enough sound, else the file is
    // deleted. Returns the path of the kept file, empty if none.
    std::string finish(bool keep) {
        std::lock_guard<std::mutex> lck(mtx);
        if (clipPath.empty()) { return ""; }

        writer.close();
        std::string path = clipPath;
        clipPath.clear();
        double audible = (double)trimmer.audibleSamples() / clipRate;
        if (keep && audible >= MIN_AUDIBLE_SECONDS) { return path; }

        std::error_code ec;
        std::filesystem::remove(path, ec);
        return "";
    }

private:
    static void handler(dsp::stereo_t* data, int count, void* ctx) {
        TransmissionRecorder* _this = (TransmissionRecorder*)ctx;
        std::lock_guard<std::mutex> lck(_this->mtx);
        if (_this->clipPath.empty()) { return; }

        float* mono = _this->monoBuf.data();
        for (int i = 0; i < count; i++) {
            mono[i] = (data[i].l + data[i].r) * 0.5f;
        }
        int kept = _this->trimmer.keep(mono, count);
        if (kept > 0) { _this->writer.write(mono, kept); }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        TransmissionRecorder* _this = (TransmissionRecorder*)ctx;
        // The radio went away, what it already said is kept
        if (name == _this->attachedName) { _this->detach(true); }
    }

    // Long enough to tell one over from the next
    static constexpr double MAX_PAUSE_SECONDS = 0.7;

    dsp::stream<dsp::stereo_t>* input = NULL;
    std::string attachedName;
    dsp::sink::Handler<dsp::stereo_t> sink;
    EventHandler<std::string> unregisterHandler;

    std::mutex mtx;
    wav::Writer writer;
    std::string clipPath;
    float clipRate = 48000.0f;
    scan::SilenceTrimmer trimmer;
    std::vector<float> monoBuf = std::vector<float>(STREAM_BUFFER_SIZE);
};
