#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "transcript.h"

#ifdef SCANNER_TRANSCRIPTION
#include <whisper.h>
#include <ggml-backend.h>
#include <dlfcn.h>
#include <filesystem>
#include <utils/flog.h>
#endif

// Speech recognition of transmissions with whisper.cpp, on a thread of its own: a transmission
// takes a few seconds, the scanner hands it over and picks the text up when it is done.
class Transcriber {
public:
    struct Result {
        int id;
        // Empty when nothing worth keeping was heard
        std::string text;
    };

    static bool available() {
#ifdef SCANNER_TRANSCRIPTION
        return true;
#else
        return false;
#endif
    }

    ~Transcriber() { stop(); }

    // Loads the model on the worker, a large one takes a few seconds
    void start(const std::string& modelPath) {
        if (!available()) { return; }
        stop();
        {
            std::lock_guard<std::mutex> lck(mtx);
            path = modelPath;
            stopping = false;
            setStatus("Loading model...");
        }
        worker = std::thread(&Transcriber::run, this);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lck(mtx);
            stopping = true;
            jobs.clear();
        }
        cnd.notify_all();
        if (worker.joinable()) { worker.join(); }
    }

    bool running() { return worker.joinable(); }

    void add(int id, std::vector<float> audio, float sampleRate) {
        {
            std::lock_guard<std::mutex> lck(mtx);
            // Falling behind on a busy frequency: the oldest waits longest, it goes
            if (jobs.size() >= MAX_QUEUE) {
                results.push_back({ jobs.front().id, "" });
                jobs.pop_front();
            }
            jobs.push_back({ id, std::move(audio), sampleRate });
        }
        cnd.notify_all();
    }

    // Results since the last call, on the GUI thread
    std::vector<Result> take() {
        std::lock_guard<std::mutex> lck(mtx);
        std::vector<Result> taken(results.begin(), results.end());
        results.clear();
        return taken;
    }

    std::string status() {
        std::lock_guard<std::mutex> lck(mtx);
        if (jobs.empty()) { return statusText; }
        return statusText + ", " + std::to_string(jobs.size()) + " waiting";
    }

private:
    struct Job {
        int id;
        std::vector<float> audio;
        float sampleRate;
    };

    void setStatus(const std::string& text) { statusText = text; }

    void run() {
#ifdef SCANNER_TRANSCRIPTION
        std::string modelPath;
        {
            std::lock_guard<std::mutex> lck(mtx);
            modelPath = path;
        }
        whisper_context* ctx = loadModel(modelPath);
        if (!ctx) { return; }

        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lck(mtx);
                cnd.wait(lck, [this] { return stopping || !jobs.empty(); });
                if (stopping) { break; }
                job = std::move(jobs.front());
                jobs.pop_front();
                setStatus("Transcribing");
            }

            std::string text = transcribe(ctx, job);

            std::lock_guard<std::mutex> lck(mtx);
            results.push_back({ job.id, text });
            setStatus("Ready");
        }
        whisper_free(ctx);
#endif
    }

#ifdef SCANNER_TRANSCRIPTION
    whisper_context* loadModel(const std::string& modelPath) {
        if (!std::filesystem::is_regular_file(modelPath)) {
            std::lock_guard<std::mutex> lck(mtx);
            setStatus("Model not found");
            return nullptr;
        }

        // ggml's CPU and Metal backends are separate libraries, an app bundle carries them
        // next to the others in Frameworks, a build from source finds them where they were
        // installed
        static std::once_flag backendsLoaded;
        std::call_once(backendsLoaded, [] {
            whisper_log_set([](enum ggml_log_level level, const char* text, void*) {
                if (level == GGML_LOG_LEVEL_ERROR) { flog::error("Whisper: {0}", text); }
            }, nullptr);
            Dl_info info;
            if (dladdr((void*)&Transcriber::available, &info) && info.dli_fname) {
                std::filesystem::path frameworks = std::filesystem::path(info.dli_fname).parent_path().parent_path() / "Frameworks";
                if (std::filesystem::exists(frameworks / "libggml-metal.so")) {
                    ggml_backend_load_all_from_path(frameworks.string().c_str());
                    return;
                }
            }
            ggml_backend_load_all();
        });

        whisper_context_params params = whisper_context_default_params();
        params.use_gpu = true;
        whisper_context* ctx = whisper_init_from_file_with_params(modelPath.c_str(), params);
        std::lock_guard<std::mutex> lck(mtx);
        setStatus(ctx ? "Ready" : "Model could not be loaded");
        return ctx;
    }

    std::string transcribe(whisper_context* ctx, const Job& job) {
        std::vector<float> samples = scan::resampleTo16k(job.audio, job.sampleRate);
        if (samples.empty()) { return ""; }

        // As tested on airband recordings: beam search, English, no prompt (a prompt gets
        // repeated back on noise)
        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        params.beam_search.beam_size = 5;
        params.greedy.best_of = 5;
        params.language = "en";
        params.translate = false;
        params.no_context = true;
        params.no_timestamps = true;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_special = false;
        params.print_timestamps = false;
        params.n_threads = THREADS;
        // Stopping doesn't wait for a long transmission to finish
        params.abort_callback = [](void* data) { return ((Transcriber*)data)->stopping.load(); };
        params.abort_callback_user_data = this;
        if (whisper_full(ctx, params, samples.data(), (int)samples.size()) != 0) { return ""; }

        std::string raw;
        int segments = whisper_full_n_segments(ctx);
        for (int i = 0; i < segments; i++) {
            raw += whisper_full_get_segment_text(ctx, i);
            raw += ' ';
        }
        return scan::formatTranscript(raw);
    }
#endif

    static constexpr size_t MAX_QUEUE = 20;
    static constexpr int THREADS = 4;

    std::mutex mtx;
    std::condition_variable cnd;
    std::thread worker;
    std::atomic<bool> stopping = false;
    std::string path;
    std::string statusText = "Off";
    std::deque<Job> jobs;
    std::deque<Result> results;
};
