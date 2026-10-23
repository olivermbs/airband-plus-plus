#pragma once
#include <signal_path/signal_path.h>
#include <dsp/channel/rx_vfo.h>
#include <dsp/sink/handler_sink.h>
#include <utils/wav.h>
#include <utils/flog.h>
#include <filesystem>
#include <mutex>

// Saves the raw signal of each recorded transmission next to its audio, the channel cut out of
// the SDR's stream by a VFO of its own: to work out offline how to receive it best. I and Q go
// to the left and right channel of a float WAV.
class IqRecorder {
public:
    static constexpr double SAMPLE_RATE = 24000.0;
    // Wide enough for the offset carriers of a 25kHz channel and its voice either side
    static constexpr double BANDWIDTH = 20000.0;

    IqRecorder() {
        vfo.init(NULL, 2.56e6, SAMPLE_RATE, BANDWIDTH, 0.0);
        sink.init(&vfo.out, handler, this);
    }

    ~IqRecorder() { disable(); }

    // Taps the SDR's stream, the VFO then runs all the time: the stream's other readers wait
    // for every one of them
    void enable() {
        if (input) { return; }
        input = new dsp::stream<dsp::complex_t>;
        inputRate = sigpath::iqFrontEnd.getEffectiveSamplerate();
        vfo.setInput(input);
        vfo.setInSamplerate(inputRate);
        sigpath::iqFrontEnd.bindIQStream(input);
        vfo.start();
        sink.start();
    }

    void disable() {
        if (!input) { return; }
        finish(false);
        sigpath::iqFrontEnd.unbindIQStream(input);
        vfo.stop();
        sink.stop();
        delete input;
        input = NULL;
    }

    bool isEnabled() { return input != NULL; }

    // Starts a capture of the channel at this offset from the SDR's center frequency
    bool start(const std::string& path, double offset) {
        if (!input) { return false; }
        finish(false);
        // The SDR's sample rate may have changed since
        double rate = sigpath::iqFrontEnd.getEffectiveSamplerate();
        if (rate != inputRate) {
            inputRate = rate;
            vfo.setInSamplerate(inputRate);
        }
        vfo.setOffset(offset);

        std::lock_guard<std::mutex> lck(mtx);
        writer.setChannels(2);
        writer.setSamplerate((uint64_t)SAMPLE_RATE);
        writer.setFormat(wav::FORMAT_WAV);
        writer.setSampleType(wav::SAMP_TYPE_FLOAT32);
        if (!writer.open(path)) {
            flog::error("Scanner: could not create {0}", path);
            return false;
        }
        clipPath = path;
        return true;
    }

    // Kept with its audio, deleted with it
    void finish(bool keep) {
        std::lock_guard<std::mutex> lck(mtx);
        if (clipPath.empty()) { return; }
        writer.close();
        if (!keep) {
            std::error_code ec;
            std::filesystem::remove(clipPath, ec);
        }
        clipPath.clear();
    }

private:
    static void handler(dsp::complex_t* data, int count, void* ctx) {
        IqRecorder* _this = (IqRecorder*)ctx;
        std::lock_guard<std::mutex> lck(_this->mtx);
        if (_this->clipPath.empty()) { return; }
        _this->writer.write((float*)data, count);
    }

    dsp::stream<dsp::complex_t>* input = NULL;
    double inputRate = 0.0;
    dsp::channel::RxVFO vfo;
    dsp::sink::Handler<dsp::complex_t> sink;

    std::mutex mtx;
    wav::Writer writer;
    std::string clipPath;
};
