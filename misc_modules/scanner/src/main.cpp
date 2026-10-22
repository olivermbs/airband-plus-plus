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
#include <fstream>
#include <map>
#include <set>
#include "channel_scan.h"

SDRPP_MOD_INFO{
    /* Name:            */ "scanner",
    /* Description:     */ "Frequency scanner for SDR++",
    /* Author:          */ "Ryzerth;olivermbs",
    /* Version:         */ 0, 4, 0,
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
        refreshBookmarkLists();
        gui::menu.registerEntry(name, menuHandler, this, NULL);

        frameHandler.handler = frameEventHandler;
        frameHandler.ctx = this;
        gui::mainWindow.onFrame.bindHandler(&frameHandler);
    }

    ~ScannerModule() {
        stop("");
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

        ImGui::LeftLabel("Scan");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##scanner_mode_" + _this->name).c_str(), &_this->mode, "Frequency Range\0Bookmarks\0")) {
            changed = true;
        }

        if (_this->mode == MODE_BOOKMARKS) {
            ImGui::LeftLabel("List");
            float refreshWidth = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX() - refreshWidth - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Combo(("##scanner_bm_list_" + _this->name).c_str(), &_this->bookmarkListId, _this->bookmarkListsTxt.c_str())) {
                _this->bookmarkList = _this->bookmarkLists[_this->bookmarkListId];
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(("Refresh##scanner_bm_refresh_" + _this->name).c_str())) {
                _this->refreshBookmarkLists();
            }
        }

        if (_this->mode == MODE_RANGE) {
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
        }
        ImGui::LeftLabel(_this->mode == MODE_RANGE ? "Step (kHz)" : "Spacing (kHz)");
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
        _this->drawExclusions();
    }

    void drawStatus() {
        if (state == State::Receiving) {
            double freq = channels[receivingChannel];
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Receiving %.5f MHz", freq / 1e6);
            ImGui::Text("SNR %.1f dB, %s", receivingSNR, followingCarrier ? "following off-channel carrier" : "on channel");
            int squelch = radioSquelchState();
            if (squelch == RADIO_IFACE_SQUELCH_NONE) {
                ImGui::TextDisabled("Holding on the FFT, use Auto squelch to hold on what you hear");
            }

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
        if (lastSweepSeconds > 0.0) {
            ImGui::Text("%d channels, full sweep %.2f s", (int)channels.size(), lastSweepSeconds);
        }
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

    void drawExclusions() {
        std::string header = "Excluded (" + std::to_string(lockouts.size()) + ")##scanner_lockouts_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) { return; }

        float addWidth = ImGui::CalcTextSize("Exclude").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputDouble(("##scanner_exclude_freq_" + name).c_str(), &excludeMHz, 0.0, 0.0, "%.5f MHz");
        ImGui::SameLine();
        if (ImGui::Button(("Exclude##scanner_exclude_" + name).c_str())) {
            lockouts.insert(std::llround(excludeMHz * 1e6));
            saveConfig();
        }

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

        if (!lockouts.empty() && ImGui::Button(("Clear All##scanner_clear_lockouts_" + name).c_str())) {
            lockouts.clear();
            saveConfig();
        }
    }

    // Lists of the frequency manager, read from its config file
    void refreshBookmarkLists() {
        bookmarkLists.clear();
        bookmarkListsTxt.clear();
        json fm = readFrequencyManagerConfig();
        if (fm.contains("lists") && fm["lists"].is_object()) {
            for (auto& [listName, list] : fm["lists"].items()) {
                bookmarkLists.push_back(listName);
                bookmarkListsTxt += listName;
                bookmarkListsTxt += '\0';
            }
        }

        auto it = std::find(bookmarkLists.begin(), bookmarkLists.end(), bookmarkList);
        bookmarkListId = (it == bookmarkLists.end()) ? 0 : (int)(it - bookmarkLists.begin());
        if (!bookmarkLists.empty()) { bookmarkList = bookmarkLists[bookmarkListId]; }
    }

    static json readFrequencyManagerConfig() {
        std::ifstream file(core::args["root"].s() + "/frequency_manager_config.json");
        json fm = json::parse(file, nullptr, false);
        return fm.is_discarded() ? json::object() : fm;
    }

    std::vector<double> bookmarkChannels() {
        std::vector<double> freqs;
        json fm = readFrequencyManagerConfig();
        if (!fm.contains("lists") || !fm["lists"].contains(bookmarkList)) { return freqs; }
        json& bookmarks = fm["lists"][bookmarkList]["bookmarks"];
        if (!bookmarks.is_object()) { return freqs; }
        for (auto& [bmName, bm] : bookmarks.items()) {
            if (bm.contains("frequency")) { freqs.push_back(bm["frequency"]); }
        }
        return freqs;
    }

    void applyAirbandPreset(double presetStep) {
        setStep(presetStep);
        startFreq = 118000000.0;
        stopFreq = (presetStep == AIRBAND_25K) ? 136975000.0 : std::round(118000000.0 + 2279.0 * step);
        lingerTime = std::max<int>(lingerTime, 3000);
        saveConfig();

        std::string vfoName = gui::waterfall.selectedVFO;
        if (core::modComManager.getModuleName(vfoName) != "radio") { return; }

        int mode = RADIO_IFACE_MODE_AM;
        float bandwidth = (presetStep == AIRBAND_25K) ? 9000.0f : 7000.0f;
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

        // Excluded channels are left out of the plan, so no time is spent on them
        std::vector<double> all = (mode == MODE_RANGE) ? scan::gridChannels({ startFreq, stopFreq, step }) : bookmarkChannels();
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        channels.clear();
        for (double freq : all) {
            if (!lockouts.count(std::llround(freq))) { channels.push_back(freq); }
        }
        if (channels.empty()) {
            error = (mode == MODE_RANGE) ? "Invalid frequency range" : "No bookmarks in this list";
            return;
        }

        // Searching is silent and only a transmission the squelch lets through is heard
        ensureLevelSquelch();
        setRadioMuted(true);

        // A faster FFT shortens every wait while scanning
        core::configManager.acquire();
        userFFTRate = core::configManager.conf["fftRate"];
        core::configManager.release();
        if (userFFTRate < SCAN_FFT_RATE) { sigpath::iqFrontEnd.setFFTRate(SCAN_FFT_RATE); }

        plannedBandwidth = 0.0;
        windowId = scanUp ? -1 : 0;
        sweepStart = Clock::now();
        lastSweepSeconds = 0.0;
        running = true;
        nextWindow();
    }

    void stop(const std::string& reason) {
        if (running && userFFTRate < SCAN_FFT_RATE) { sigpath::iqFrontEnd.setFFTRate(userFFTRate); }
        if (running) { setRadioMuted(false); }
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
        if (state == State::Tuning) {
            if (!newFrameAvailable()) { return; }
            enterSearching();
        }
        if (!readSpectrum()) { return; }

        if (state == State::Receiving) {
            updateReceiving(now);
            return;
        }

        search(now);
    }

    // Copies the newest FFT frame. Frames that were already used, and frames that may still
    // hold samples from before the last retune, are rejected.
    bool newFrameAvailable() {
        int size = 0;
        uint64_t frame = 0;
        if (!gui::waterfall.acquireRawFFT(size, frame)) { return false; }
        gui::waterfall.releaseRawFFT();
        return frame != lastFrame && frame >= tuneFrame + FRAMES_AFTER_TUNE;
    }

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

        // Searching averages the frames of the window, receiving looks at the newest frame only
        int factor = scan::detectionAveraging(step, rawBinWidth);
        if (state != State::Searching) { detection.reset(); }
        detection.add(rawFFT.data(), size, factor);
        const std::vector<float>& bins = detection.bins();
        detectionSpectrum = { bins.data(), (int)bins.size(), start, rawBinWidth * factor * bins.size(), center };
        return true;
    }

    void search(Clock::time_point now) {
        const scan::Window& window = windows[windowId];
        int count = window.last - window.first + 1;
        std::vector<float> snr(count);
        for (int i = 0; i < count; i++) {
            snr[i] = isSkipped(channels[window.first + i], now) ? -INFINITY : channelSNR(window.first + i);
        }

        // A decision needs at least two frames averaged, one frame is too noisy
        std::vector<int> averagedFrames(count, detection.frameCount());
        int picked = scan::pickStrongest(snr, averagedFrames, MIN_AVERAGED_FRAMES, trigger);
        if (picked < 0) {
            searchFrames++;
            if (searchFrames >= SEARCH_FRAMES) { nextWindow(); }
            return;
        }

        // Name the channel after where the carrier actually is
        double center = carrierCenter(window.first + picked);
        int channel = std::clamp<int>(scan::nearestChannel(channels, center), window.first, window.last);
        listen(channel, snr[picked], center, now);
    }

    void updateReceiving(Clock::time_point now) {
        // The user tuned away: stop interfering and stay where they put the VFO
        double actualFreq = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos[vfoName]->centerOffset;
        if (std::fabs(actualFreq - vfoFreq) > 1.0) {
            hold = true;
            vfoFreq = actualFreq;
        }

        // The radio's squelch decides what is heard, so it also decides how long to stay.
        // Without a level squelch the FFT is used instead.
        receivingSNR = channelSNR(receivingChannel);
        // Stay while either hears it: a weak reply from the ground may stay under the squelch
        // but still shows in the FFT
        int squelch = radioSquelchState();
        bool present = (squelch == RADIO_IFACE_SQUELCH_OPEN) || (receivingSNR >= trigger - RECEIVE_HYSTERESIS);
        if (present) {
            lastSignal = now;
            confirmedAfterListen = true;
        }
        if (hold) { return; }

        // Nothing in the frames after locking on: it was a noise spike or a stale frame from before
        // the retune, don't wait for the linger time
        framesSinceListen++;
        if (!confirmedAfterListen && framesSinceListen >= VERIFY_FRAMES) {
            resumeSearching();
            return;
        }

        if (followingCarrier && receivingSNR >= trigger) {
            followCarrier(carrierCenter(receivingChannel));
        }

        if (maxListenTime > 0 && now - receiveStart > std::chrono::seconds(maxListenTime)) {
            tempSkips[std::llround(channels[receivingChannel])] = now + std::chrono::seconds(SKIP_TIME_S);
            resumeSearching();
            return;
        }

        if (now - lastSignal > std::chrono::milliseconds(lingerTime)) {
            resumeSearching();
        }
    }

    bool radioAvailable() {
        return core::modComManager.getModuleName(vfoName) == "radio";
    }

    // Without a level squelch every channel passed would be heard as noise
    void ensureLevelSquelch() {
        if (!radioAvailable()) { return; }
        SquelchMode squelchMode = SQUELCH_MODE_OFF;
        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_SQUELCH_MODE, NULL, &squelchMode);
        if (squelchMode == SQUELCH_MODE_POWER || squelchMode == SQUELCH_MODE_AUTO) { return; }
        squelchMode = SQUELCH_MODE_AUTO;
        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH_MODE, &squelchMode, NULL);
    }

    void setRadioMuted(bool muted) {
        if (!radioAvailable()) { return; }
        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH_MUTE, &muted, NULL);
    }

    int radioSquelchState() {
        int state = RADIO_IFACE_SQUELCH_NONE;
        if (radioAvailable()) {
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_SQUELCH_STATE, NULL, &state);
        }
        return state;
    }

    double detectionHalfWidth() {
        return step * (detectionWidth / 100.0) / 2.0;
    }

    double dcGuard() {
        return detectionSpectrum.binWidth() * 1.5;
    }

    float channelSNR(int channel) {
        double freq = channels[channel];
        float peak = scan::channelPeak(detectionSpectrum, freq, detectionHalfWidth(), dcGuard());
        float floor = scan::noiseFloor(detectionSpectrum, freq, FLOOR_RADIUS, detectionHalfWidth(), dcGuard(), FLOOR_QUANTILE);
        return peak - floor;
    }

    // Centered on the full resolution FFT, a carrier is located to within a few Hz
    double carrierCenter(int channel) {
        return scan::signalCenter(fineSpectrum, channels[channel], detectionHalfWidth(), dcGuard());
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
        flog::info("Scanner: receiving {0:.5f} MHz, carrier at {1:.5f} MHz, {2:.1f} dB", channels[channel] / 1e6, center / 1e6, snr);
        state = State::Receiving;
        receivingChannel = channel;
        receivingSNR = snr;
        receiveStart = now;
        lastSignal = now;
        hold = false;
        framesSinceListen = 0;
        confirmedAfterListen = false;

        // Aircraft are often a few hundred Hz to over a kHz off, the ground station is usually
        // spot on. Listening on the channel itself hears both sides of the conversation, only a
        // carrier that is really off the channel is followed.
        followingCarrier = std::fabs(center - channels[channel]) > step * CARRIER_FOLLOW_OFFSET;
        trackedCenter = followingCarrier ? center : channels[channel];
        tuneVFO(trackedCenter);
        setRadioMuted(false);
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
        if (state == State::Receiving) { setRadioMuted(true); }
        state = State::Searching;
        searchFrames = 0;
        detection.reset();
    }

    void nextWindow() {
        if (gui::waterfall.getBandwidth() != plannedBandwidth) {
            planWindows();
        }

        int count = (int)windows.size();
        int next = ((windowId + (scanUp ? 1 : -1)) % count + count) % count;
        bool wrapped = scanUp ? (next == 0) : (next == count - 1);
        if (wrapped && windowId >= 0) {
            auto now = Clock::now();
            lastSweepSeconds = std::chrono::duration<double>(now - sweepStart).count();
            sweepStart = now;
        }
        windowId = next;
        tuneWindow();
    }

    void replanAndTune() {
        double freq = channels[windows[windowId].first];
        planWindows();
        windowId = 0;
        for (int i = 0; i < (int)windows.size(); i++) {
            if (channels[windows[i].last] >= freq) {
                windowId = i;
                break;
            }
        }
        tuneWindow();
    }

    void planWindows() {
        plannedBandwidth = gui::waterfall.getBandwidth();

        // On a regular grid DC goes exactly between two channels, bookmarks can be anywhere so
        // DC is kept clear of the whole detection width
        double dcClearance = (mode == MODE_RANGE) ? step / 2.0 : detectionHalfWidth() + DC_CLEARANCE_MARGIN;
        windows = scan::planWindows(channels, plannedBandwidth * USABLE_BANDWIDTH, dcClearance);
    }

    void tuneWindow() {
        const scan::Window& window = windows[windowId];
        gui::waterfall.setCenterFrequency(window.center);
        gui::waterfall.centerFreqMoved = true;
        sigpath::sourceManager.tune(window.center);
        tuneVFO(channels[window.first]);

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
        if (conf.contains("mode")) { mode = conf["mode"]; }
        if (conf.contains("bookmarkList")) { bookmarkList = conf["bookmarkList"]; }
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
        conf["mode"] = mode;
        conf["bookmarkList"] = bookmarkList;
        conf["lockouts"] = json::array();
        for (long long freq : lockouts) {
            conf["lockouts"].push_back(freq);
        }
        config.release(true);
    }

    static constexpr double AIRBAND_25K = 25000.0;
    static constexpr double AIRBAND_833 = 25000.0 / 3.0;

    static constexpr int MODE_RANGE = 0;
    static constexpr int MODE_BOOKMARKS = 1;

    // Every new FFT frame is evaluated, the tick only bounds how often the FFT is polled
    static constexpr int TICK_MS = 5;
    static constexpr int SEARCH_FRAMES = 3;
    static constexpr int VERIFY_FRAMES = 3;
    static constexpr int SCAN_FFT_RATE = 60;
    static constexpr double DC_CLEARANCE_MARGIN = 2000.0;
    static constexpr int MIN_AVERAGED_FRAMES = 2;
    static constexpr uint64_t FRAMES_AFTER_TUNE = 3;
    static constexpr int SKIP_TIME_S = 30;
    static constexpr float RECEIVE_HYSTERESIS = 3.0f;
    static constexpr double CENTER_SMOOTHING = 0.3;
    static constexpr double MIN_RETUNE_HZ = 200.0;
    // Fraction of the step a carrier may be off the channel before it is followed instead
    static constexpr double CARRIER_FOLLOW_OFFSET = 0.25;

    // Only the flat middle of the spectrum is used, the edges roll off
    static constexpr double USABLE_BANDWIDTH = 0.85;
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
    int tuningTime = 30;
    int lingerTime = 3000;
    int maxListenTime = 120;
    std::set<long long> lockouts;
    std::map<long long, Clock::time_point> tempSkips;

    bool running = false;
    std::string error;
    std::string vfoName;
    bool scanUp = true;
    bool hold = false;
    Clock::time_point lastTick;

    int mode = MODE_RANGE;
    std::string bookmarkList;
    std::vector<std::string> bookmarkLists;
    std::string bookmarkListsTxt;
    int bookmarkListId = 0;
    double excludeMHz = 121.5;

    std::vector<double> channels;
    int userFFTRate = 20;
    Clock::time_point sweepStart;
    double lastSweepSeconds = 0.0;
    std::vector<scan::Window> windows;
    double plannedBandwidth = 0.0;
    int windowId = 0;

    std::vector<float> rawFFT;
    scan::DetectionAverage detection;
    scan::Spectrum fineSpectrum = {};
    scan::Spectrum detectionSpectrum = {};
    double rawBinWidth = 0.0;
    uint64_t lastFrame = 0;
    uint64_t tuneFrame = 0;

    State state = State::Tuning;
    Clock::time_point stateSince;
    int searchFrames = 0;

    int receivingChannel = 0;
    int framesSinceListen = 0;
    bool confirmedAfterListen = false;
    float receivingSNR = 0.0f;
    double vfoFreq = 0.0;
    double trackedCenter = 0.0;
    bool followingCarrier = false;
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
