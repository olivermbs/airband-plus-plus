#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <map>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <utils/event.h>

class SourceManager {
public:
    SourceManager();

    struct SourceHandler {
        dsp::stream<dsp::complex_t>* stream;
        void (*menuHandler)(void* ctx);
        void (*selectHandler)(void* ctx);
        void (*deselectHandler)(void* ctx);
        void (*startHandler)(void* ctx);
        void (*stopHandler)(void* ctx);
        void (*tuneHandler)(double freq, void* ctx);
        void* ctx;
        // The hardware takes a retune on its own thread and reports it with reportTuned()
        bool asyncTuning = false;
    };

    enum TuningMode {
        NORMAL,
        PANADAPTER
    };

    void registerSource(std::string name, SourceHandler* handler);
    void unregisterSource(std::string name);
    void selectSource(std::string name);
    void showSelectedMenu();
    void start();
    void stop();
    void tune(double freq);
    void setTuningOffset(double offset);
    void setTuningMode(TuningMode mode);
    void setPanadapterIF(double freq);

    std::vector<std::string> getSourceNames();

    // Sources that tune asynchronously report a retune the hardware didn't take, so whoever
    // relies on the new frequency (e.g. a scanner) knows the spectrum still shows the old one
    void reportTuneFailed() { tuneFailures++; }
    uint64_t getTuneFailures() { return tuneFailures; }

    // Every retune is numbered. The spectrum shows the newest frequency only once the hardware
    // took the newest request, getTunesDone() == getTuneRequests().
    uint64_t getTuneRequests() { return tuneRequests; }
    uint64_t getTunesDone() { return tunesDone; }
    void reportTuned(uint64_t request) {
        uint64_t done = tunesDone;
        while (done < request && !tunesDone.compare_exchange_weak(done, request)) {}
    }

    Event<std::string> onSourceRegistered;
    Event<std::string> onSourceUnregister;
    Event<std::string> onSourceUnregistered;
    Event<double> onRetune;

private:
    std::map<std::string, SourceHandler*> sources;
    std::string selectedName;
    SourceHandler* selectedHandler = NULL;
    double tuneOffset;
    double currentFreq;
    double ifFreq = 0.0;
    TuningMode tuneMode = TuningMode::NORMAL;
    std::atomic<uint64_t> tuneFailures = 0;
    std::atomic<uint64_t> tuneRequests = 0;
    std::atomic<uint64_t> tunesDone = 0;
    dsp::stream<dsp::complex_t> nullSource;
};