#include <imgui.h>
#include <module.h>
#include <core.h>
#include <config.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <radio_interface.h>
#include <utils/flog.h>
#include <chrono>
#include <map>
#include <set>
#include "channel_scan.h"

SDRPP_MOD_INFO{
    /* Name:            */ "scanner",
    /* Description:     */ "Frequency scanner for SDR++",
    /* Author:          */ "Ryzerth;olivermbs",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1
};

ConfigManager config;

using Clock = std::chrono::steady_clock;

// The scanner runs entirely on the GUI thread, from the main window's frame event. That is
// where VFOs are created and deleted and where tuning normally happens, so no locking is needed.
class ScannerModule : public ModuleManager::Instance {
public:
    ScannerModule(std::string name) {
        this->name = name;
        loadConfig();
        gui::menu.registerEntry(name, menuHandler, this, NULL);

        frameHandler.handler = frameEventHandler;
        frameHandler.ctx = this;
        gui::mainWindow.onFrame.bindHandler(&frameHandler);
    }

    ~ScannerModule() {
        gui::mainWindow.onFrame.unbindHandler(&frameHandler);
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        stop("");
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    enum class State {
        Tuning,
        Searching,
        Receiving
    };

    static void menuHandler(void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;
        bool changed = false;

        if (_this->running) { ImGui::BeginDisabled(); }
        ImGui::BeginTable(("scanner_preset_table" + _this->name).c_str(), 2);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Airband 25kHz##scanner_ab25_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->applyAirbandPreset(AIRBAND_25K);
        }
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(("Airband 8.33kHz##scanner_ab833_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->applyAirbandPreset(AIRBAND_833);
        }
        ImGui::EndTable();

        ImGui::LeftLabel("Start (MHz)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        double startMHz = _this->startFreq / 1e6;
        if (ImGui::InputDouble(("##start_freq_scanner_" + _this->name).c_str(), &startMHz, 0.025, 1.0, "%.5f")) {
            _this->startFreq = std::round(startMHz * 1e6);
            changed = true;
        }
        ImGui::LeftLabel("Stop (MHz)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        double stopMHz = _this->stopFreq / 1e6;
        if (ImGui::InputDouble(("##stop_freq_scanner_" + _this->name).c_str(), &stopMHz, 0.025, 1.0, "%.5f")) {
            _this->stopFreq = std::round(stopMHz * 1e6);
            changed = true;
        }
        ImGui::LeftLabel("Step (kHz)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        double stepKHz = _this->step / 1e3;
        if (ImGui::InputDouble(("##step_scanner_" + _this->name).c_str(), &stepKHz, 0.0, 0.0, "%.3f")) {
            _this->setStep(stepKHz * 1e3);
            changed = true;
        }
        if (_this->running) { ImGui::EndDisabled(); }

        ImGui::LeftLabel("Trigger (dB SNR)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(("##trigger_scanner_" + _this->name).c_str(), &_this->trigger, 3.0f, 30.0f, "%.1f")) {
            changed = true;
        }
        ImGui::LeftLabel("Detection Width (%)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(("##det_width_scanner_" + _this->name).c_str(), &_this->detectionWidth, 10.0f, 100.0f, "%.0f")) {
            changed = true;
        }
        ImGui::LeftLabel("Tuning Time (ms)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##tuning_time_scanner_" + _this->name).c_str(), &_this->tuningTime, 50, 500)) {
            _this->tuningTime = std::clamp<int>(_this->tuningTime, 0, 10000);
            changed = true;
        }
        ImGui::LeftLabel("Linger Time (ms)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##linger_time_scanner_" + _this->name).c_str(), &_this->lingerTime, 100, 1000)) {
            _this->lingerTime = std::clamp<int>(_this->lingerTime, 0, 60000);
            changed = true;
        }
        ImGui::LeftLabel("Max Listen (s, 0=off)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##max_listen_scanner_" + _this->name).c_str(), &_this->maxListenTime, 10, 60)) {
            _this->maxListenTime = std::clamp<int>(_this->maxListenTime, 0, 3600);
            changed = true;
        }
        if (changed) { _this->saveConfig(); }

        ImGui::BeginTable(("scanner_bottom_btn_table" + _this->name).c_str(), 2);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("<<##scanner_back_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->scanUp = false;
            _this->skipAhead();
        }
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button((">>##scanner_forw_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->scanUp = true;
            _this->skipAhead();
        }
        ImGui::EndTable();

        if (!_this->running) {
            if (ImGui::Button(("Start##scanner_start_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->start();
            }
            if (!_this->error.empty()) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", _this->error.c_str());
            }
            else {
                ImGui::TextUnformatted("Status: Idle");
            }
        }
        else {
            if (ImGui::Button(("Stop##scanner_start_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->stop("");
            }
            _this->drawStatus();
        }

        _this->drawWarnings();
        _this->drawLockouts();
    }

    void drawStatus() {
        if (state == State::Receiving) {
            double freq = grid.freq(receivingChannel);
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Receiving %.5f MHz", freq / 1e6);
            ImGui::Text("SNR %.1f dB, carrier %+.2f kHz", receivingSNR, (vfoFreq - freq) / 1e3);

            ImGui::BeginTable(("scanner_rx_btn_table" + name).c_str(), 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button(("Skip##scanner_skip_" + name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                tempSkips[std::llround(freq)] = Clock::now() + std::chrono::seconds(SKIP_TIME_S);
                resumeSearching();
            }
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(("Lock Out##scanner_lockout_" + name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                lockouts.insert(std::llround(freq));
                saveConfig();
                resumeSearching();
            }
            ImGui::EndTable();

            ImGui::Checkbox(("Hold##scanner_hold_" + name).c_str(), &hold);
            return;
        }

        if (state == State::Tuning) {
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Tuning window %d/%d", windowId + 1, (int)windows.size());
            return;
        }

        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scanning window %d/%d", windowId + 1, (int)windows.size());
    }

    void drawWarnings() {
        core::configManager.acquire();
        bool centerTuning = core::configManager.conf["centerTuning"];
        core::configManager.release();
        if (centerTuning) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Center tuning is on, switch it off");
        }

        if (rawBinWidth > step / 4.0) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "FFT too coarse (%.1f kHz/bin)", rawBinWidth / 1e3);
            ImGui::TextWrapped("Increase the FFT size in the display settings.");
        }
    }

    void drawLockouts() {
        if (lockouts.empty()) { return; }

        if (!ImGui::CollapsingHeader(("Locked out (" + std::to_string(lockouts.size()) + ")##scanner_lockouts_" + name).c_str())) { return; }

        long long removed = -1;
        for (long long freq : lockouts) {
            ImGui::Text("%.5f MHz", (double)freq / 1e6);
            ImGui::SameLine();
            if (ImGui::SmallButton(("Remove##scanner_unlock_" + std::to_string(freq)).c_str())) {
                removed = freq;
            }
        }

        if (removed >= 0) {
            lockouts.erase(removed);
            saveConfig();
        }

        if (ImGui::Button(("Clear All##scanner_clear_lockouts_" + name).c_str())) {
            lockouts.clear();
            saveConfig();
        }
    }

    void applyAirbandPreset(double presetStep) {
        setStep(presetStep);
        startFreq = 118000000.0;
        stopFreq = (presetStep == AIRBAND_25K) ? 136975000.0 : std::round(118000000.0 + 2279.0 * step);
        saveConfig();

        std::string vfoName = gui::waterfall.selectedVFO;
        if (core::modComManager.getModuleName(vfoName) != "radio") { return; }

        int mode = RADIO_IFACE_MODE_AM;
        float bandwidth = (presetStep == AIRBAND_25K) ? 9000.0f : 6000.0f;
        SquelchMode squelchMode = SQUELCH_MODE_AUTO;
        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH_MODE, &squelchMode, NULL);
    }

    // 8.33kHz channels are exactly a third of 25kHz, which can't be typed in exactly
    void setStep(double newStep) {
        step = (std::fabs(newStep - AIRBAND_833) < 1.0) ? AIRBAND_833 : std::max<double>(100.0, newStep);
    }

    void start() {
        if (running) { return; }

        error.clear();
        vfoName = gui::waterfall.selectedVFO;
        if (vfoName.empty()) {
            error = "No VFO selected";
            return;
        }
        if (!gui::mainWindow.sdrIsRunning()) {
            error = "Start the SDR first";
            return;
        }

        grid = { startFreq, stopFreq, step };
        if (grid.count() == 0) {
            error = "Invalid frequency range";
            return;
        }

        plannedBandwidth = 0.0;
        windowId = scanUp ? -1 : 0;
        running = true;
        nextWindow();
    }

    void stop(const std::string& reason) {
        running = false;
        error = reason;
    }

    static void frameEventHandler(bool playing, void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        if (!_this->running) { return; }

        auto now = Clock::now();
        if (now - _this->lastTick < std::chrono::milliseconds(TICK_MS)) { return; }
        _this->lastTick = now;
        _this->tick(playing, now);
    }

    void tick(bool playing, Clock::time_point now) {
        if (!playing) {
            stop("Stopped: the SDR was stopped");
            return;
        }
        if (gui::waterfall.selectedVFO != vfoName || gui::waterfall.vfos.find(vfoName) == gui::waterfall.vfos.end()) {
            stop("Stopped: the VFO changed");
            return;
        }

        // Retune if the user moved the SDR or changed its sample rate
        if (gui::waterfall.getBandwidth() != plannedBandwidth || std::fabs(gui::waterfall.getCenterFrequency() - windows[windowId].center) > 1.0) {
            replanAndTune();
            return;
        }

        if (state == State::Tuning && now - stateSince < std::chrono::milliseconds(tuningTime)) { return; }
        if (!readSpectrum()) { return; }
        if (state == State::Tuning) { enterSearching(); }

        if (state == State::Receiving) {
            updateReceiving(now);
            return;
        }

        search(now);
    }

    // Copies the newest FFT frame. Frames that were already used, and frames that may still
    // hold samples from before the last retune, are rejected.
    bool readSpectrum() {
        int size = 0;
        uint64_t frame = 0;
        float* raw = gui::waterfall.acquireRawFFT(size, frame);
        if (!raw) { return false; }
        if (frame == lastFrame || frame < tuneFrame + FRAMES_AFTER_TUNE) {
            gui::waterfall.releaseRawFFT();
            return false;
        }
        lastFrame = frame;
        rawFFT.assign(raw, raw + size);
        gui::waterfall.releaseRawFFT();

        double center = gui::waterfall.getCenterFrequency();
        rawBinWidth = plannedBandwidth / (double)size;
        double start = center - (plannedBandwidth / 2.0) - (rawBinWidth / 2.0);
        fineSpectrum = { rawFFT.data(), size, start, plannedBandwidth, center };

        int factor = std::max<int>(1, (int)std::floor(step / DETECTION_BINS_PER_STEP / rawBinWidth));
        detectionBins = scan::averageBins(rawFFT.data(), size, factor);
        detectionSpectrum = { detectionBins.data(), (int)detectionBins.size(), start, rawBinWidth * factor * detectionBins.size(), center };
        return true;
    }

    void search(Clock::time_point now) {
        const scan::Window& window = windows[windowId];
        int count = window.last - window.first + 1;
        std::vector<float> snr(count);
        for (int i = 0; i < count; i++) {
            snr[i] = isSkipped(grid.freq(window.first + i), now) ? -INFINITY : channelSNR(window.first + i);
            hits[i] = (snr[i] >= trigger) ? hits[i] + 1 : 0;
        }

        int picked = scan::pickStrongest(snr, hits, CONFIRM_FRAMES, trigger);
        if (picked < 0) {
            searchFrames++;
            if (searchFrames >= SEARCH_FRAMES) { nextWindow(); }
            return;
        }

        // Name the channel after where the carrier actually is
        double center = carrierCenter(window.first + picked);
        int channel = std::clamp<int>(grid.indexOf(center), window.first, window.last);
        listen(channel, snr[picked], center, now);
    }

    void updateReceiving(Clock::time_point now) {
        // The user tuned away: stop interfering and stay where they put the VFO
        double actualFreq = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos[vfoName]->centerOffset;
        if (std::fabs(actualFreq - vfoFreq) > 1.0) {
            hold = true;
            vfoFreq = actualFreq;
        }

        receivingSNR = channelSNR(receivingChannel);
        if (receivingSNR >= trigger - RECEIVE_HYSTERESIS) {
            lastSignal = now;
        }
        if (hold) { return; }

        if (receivingSNR >= trigger) {
            followCarrier(carrierCenter(receivingChannel));
        }

        if (maxListenTime > 0 && now - receiveStart > std::chrono::seconds(maxListenTime)) {
            tempSkips[std::llround(grid.freq(receivingChannel))] = now + std::chrono::seconds(SKIP_TIME_S);
            resumeSearching();
            return;
        }

        if (now - lastSignal > std::chrono::milliseconds(lingerTime)) {
            resumeSearching();
        }
    }

    double detectionHalfWidth() {
        return step * (detectionWidth / 100.0) / 2.0;
    }

    double dcGuard() {
        return detectionSpectrum.binWidth() * 1.5;
    }

    float channelSNR(int channel) {
        double freq = grid.freq(channel);
        float peak = scan::channelPeak(detectionSpectrum, freq, detectionHalfWidth(), dcGuard());
        float floor = scan::noiseFloor(detectionSpectrum, freq, FLOOR_RADIUS, detectionHalfWidth(), dcGuard(), FLOOR_QUANTILE);
        return peak - floor;
    }

    // Centered on the full resolution FFT, a carrier is located to within a few Hz
    double carrierCenter(int channel) {
        return scan::signalCenter(fineSpectrum, grid.freq(channel), detectionHalfWidth(), dcGuard());
    }

    // Keeps the VFO in the middle of the carrier, smoothed so the audio doesn't jump around
    void followCarrier(double center) {
        trackedCenter += CENTER_SMOOTHING * (center - trackedCenter);
        if (std::fabs(trackedCenter - vfoFreq) < MIN_RETUNE_HZ) { return; }
        tuneVFO(trackedCenter);
    }

    bool isSkipped(double freq, Clock::time_point now) {
        long long key = std::llround(freq);
        if (lockouts.count(key)) { return true; }

        auto it = tempSkips.find(key);
        if (it == tempSkips.end()) { return false; }
        if (it->second > now) { return true; }

        tempSkips.erase(it);
        return false;
    }

    void listen(int channel, float snr, double center, Clock::time_point now) {
        state = State::Receiving;
        receivingChannel = channel;
        receivingSNR = snr;
        receiveStart = now;
        lastSignal = now;
        hold = false;
        trackedCenter = center;
        tuneVFO(center);
    }

    void skipAhead() {
        if (!running) { return; }
        if (state == State::Receiving) {
            resumeSearching();
            return;
        }
        nextWindow();
    }

    void resumeSearching() {
        if (!running || state != State::Receiving) { return; }
        enterSearching();
    }

    void enterSearching() {
        state = State::Searching;
        searchFrames = 0;
        hits.assign(windows[windowId].last - windows[windowId].first + 1, 0);
    }

    void nextWindow() {
        if (gui::waterfall.getBandwidth() != plannedBandwidth) {
            planWindows();
        }

        int count = (int)windows.size();
        windowId = ((windowId + (scanUp ? 1 : -1)) % count + count) % count;
        tuneWindow();
    }

    void replanAndTune() {
        double freq = grid.freq(windows[windowId].first);
        planWindows();
        windowId = 0;
        for (int i = 0; i < (int)windows.size(); i++) {
            if (grid.freq(windows[i].last) >= freq) {
                windowId = i;
                break;
            }
        }
        tuneWindow();
    }

    void planWindows() {
        plannedBandwidth = gui::waterfall.getBandwidth();
        windows = scan::planWindows(grid, plannedBandwidth * USABLE_BANDWIDTH);
    }

    void tuneWindow() {
        const scan::Window& window = windows[windowId];
        gui::waterfall.setCenterFrequency(window.center);
        gui::waterfall.centerFreqMoved = true;
        sigpath::sourceManager.tune(window.center);
        tuneVFO(grid.freq(window.first));

        int size;
        if (gui::waterfall.acquireRawFFT(size, tuneFrame)) {
            gui::waterfall.releaseRawFFT();
        }

        state = State::Tuning;
        stateSince = Clock::now();
    }

    void tuneVFO(double freq) {
        vfoFreq = freq;
        sigpath::vfoManager.setCenterOffset(vfoName, freq - gui::waterfall.getCenterFrequency());
    }

    void loadConfig() {
        config.acquire();
        json& conf = config.conf;
        if (conf.contains("startFreq")) { startFreq = conf["startFreq"]; }
        if (conf.contains("stopFreq")) { stopFreq = conf["stopFreq"]; }
        if (conf.contains("step")) { setStep(conf["step"]); }
        if (conf.contains("trigger")) { trigger = conf["trigger"]; }
        if (conf.contains("detectionWidth")) { detectionWidth = conf["detectionWidth"]; }
        if (conf.contains("tuningTime")) { tuningTime = conf["tuningTime"]; }
        if (conf.contains("lingerTime")) { lingerTime = conf["lingerTime"]; }
        if (conf.contains("maxListenTime")) { maxListenTime = conf["maxListenTime"]; }
        if (conf.contains("lockouts")) {
            for (long long freq : conf["lockouts"]) {
                lockouts.insert(freq);
            }
        }
        config.release();
    }

    void saveConfig() {
        config.acquire();
        json& conf = config.conf;
        conf["startFreq"] = startFreq;
        conf["stopFreq"] = stopFreq;
        conf["step"] = step;
        conf["trigger"] = trigger;
        conf["detectionWidth"] = detectionWidth;
        conf["tuningTime"] = tuningTime;
        conf["lingerTime"] = lingerTime;
        conf["maxListenTime"] = maxListenTime;
        conf["lockouts"] = json::array();
        for (long long freq : lockouts) {
            conf["lockouts"].push_back(freq);
        }
        config.release(true);
    }

    static constexpr double AIRBAND_25K = 25000.0;
    static constexpr double AIRBAND_833 = 25000.0 / 3.0;

    static constexpr int TICK_MS = 50;
    static constexpr int SEARCH_FRAMES = 4;
    static constexpr int CONFIRM_FRAMES = 2;
    static constexpr uint64_t FRAMES_AFTER_TUNE = 2;
    static constexpr int SKIP_TIME_S = 30;
    static constexpr float RECEIVE_HYSTERESIS = 3.0f;
    static constexpr double CENTER_SMOOTHING = 0.3;
    static constexpr double MIN_RETUNE_HZ = 200.0;

    // Only the flat middle of the spectrum is used, the edges roll off
    static constexpr double USABLE_BANDWIDTH = 0.85;
    static constexpr double DETECTION_BINS_PER_STEP = 12.0;
    static constexpr double FLOOR_RADIUS = 150000.0;
    static constexpr float FLOOR_QUANTILE = 0.3f;

    std::string name;
    bool enabled = true;
    EventHandler<bool> frameHandler;

    double startFreq = 118000000.0;
    double stopFreq = 136975000.0;
    double step = AIRBAND_25K;
    float trigger = 8.0f;
    float detectionWidth = 70.0f;
    int tuningTime = 100;
    int lingerTime = 2000;
    int maxListenTime = 120;
    std::set<long long> lockouts;
    std::map<long long, Clock::time_point> tempSkips;

    bool running = false;
    std::string error;
    std::string vfoName;
    bool scanUp = true;
    bool hold = false;
    Clock::time_point lastTick;

    scan::ChannelGrid grid = { 0.0, 0.0, 0.0 };
    std::vector<scan::Window> windows;
    double plannedBandwidth = 0.0;
    int windowId = 0;

    std::vector<float> rawFFT;
    std::vector<float> detectionBins;
    scan::Spectrum fineSpectrum = {};
    scan::Spectrum detectionSpectrum = {};
    double rawBinWidth = 0.0;
    uint64_t lastFrame = 0;
    uint64_t tuneFrame = 0;

    State state = State::Tuning;
    Clock::time_point stateSince;
    int searchFrames = 0;
    std::vector<int> hits;

    int receivingChannel = 0;
    float receivingSNR = 0.0f;
    double vfoFreq = 0.0;
    double trackedCenter = 0.0;
    Clock::time_point receiveStart;
    Clock::time_point lastSignal;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/scanner_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ScannerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ScannerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
