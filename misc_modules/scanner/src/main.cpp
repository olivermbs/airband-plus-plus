#include <imgui.h>
#include <module.h>
#include <core.h>
#include <config.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/main_window.h>
#include <gui/widgets/folder_select.h>
#include <signal_path/signal_path.h>
#include <radio_interface.h>
#include <dsp/noise_reduction/spectrum_noise.h>
#include <utils/flog.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif
#include "channel_scan.h"
#include "clip_player.h"
#include "iq_recorder.h"
#include "recording.h"
#include "transmission_recorder.h"

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
    ScannerModule(std::string name) : recordFolder(DEFAULT_RECORD_FOLDER) {
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

    void postInit() {
        player.selectDefaultSink();
    }

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

        _this->drawControls(menuWidth);

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
            // Lists created in the frequency manager show up without having to refresh
            if (Clock::now() - _this->lastListRefresh > std::chrono::seconds(LIST_REFRESH_S)) { _this->refreshBookmarkLists(); }

            ImGui::LeftLabel("List");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##scanner_bm_list_" + _this->name).c_str(), &_this->bookmarkListId, _this->bookmarkListsTxt.c_str())) {
                _this->bookmarkList = _this->bookmarkLists[_this->bookmarkListId];
                changed = true;
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
        ImGui::LeftLabel("Reply Watch (kHz)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(("##reply_watch_scanner_" + _this->name).c_str(), &_this->replyWatchKHz, 0.0f, 25.0f, _this->replyWatchKHz > 0.0f ? "+-%.1f" : "off")) {
            changed = true;
        }
        ImGui::LeftLabel("Max Listen (s, 0=off)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##max_listen_scanner_" + _this->name).c_str(), &_this->maxListenTime, 10, 60)) {
            _this->maxListenTime = std::clamp<int>(_this->maxListenTime, 0, 3600);
            changed = true;
        }
        if (changed) { _this->saveConfig(); }

        _this->drawWarnings();
        _this->drawPriority();
        _this->drawRecording();
        _this->drawActivity();
        _this->drawAutoSkips();
        _this->drawExclusions();
    }

    // Always the same number of rows whatever the state, so nothing below jumps around
    void drawControls(float menuWidth) {
        // A lock only counts as receiving once something can be heard, until then it is still scanning
        bool receiving = running && state == State::Receiving && confirmedAfterListen;
        float half = (menuWidth - ImGui::GetStyle().ItemSpacing.x) / 2.0f;

        if (!running) {
            if (ImGui::Button(("Start##scanner_start_" + name).c_str(), ImVec2(half, 0))) { start(); }
        }
        else {
            if (ImGui::Button(("Stop##scanner_start_" + name).c_str(), ImVec2(half, 0))) { stop(""); }
        }
        ImGui::SameLine();
        if (!receiving) { ImGui::BeginDisabled(); }
        // The click changes hold, the color is popped as it was pushed: a pop without its push
        // broke the colors of the whole interface
        bool holding = hold;
        if (holding) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.0f, 1.0f)); }
        if (ImGui::Button((std::string(holding ? "Holding" : "Hold") + "##scanner_hold_" + name).c_str(), ImVec2(half, 0))) { hold = !hold; }
        if (holding) { ImGui::PopStyleColor(); }

        double freq = receiving ? receivingFreq : 0.0;
        if (ImGui::Button(("Skip##scanner_skip_" + name).c_str(), ImVec2(half, 0))) {
            tempSkips[std::llround(freq)] = Clock::now() + std::chrono::seconds(SKIP_TIME_S);
            resumeSearching();
        }
        ImGui::SameLine();
        if (ImGui::Button(("Lock Out##scanner_lockout_" + name).c_str(), ImVec2(half, 0))) {
            lockouts.insert(std::llround(freq));
            saveConfig();
            resumeSearching();
        }
        if (!receiving) { ImGui::EndDisabled(); }

        if (ImGui::Button(("<<##scanner_back_" + name).c_str(), ImVec2(half, 0))) {
            scanUp = false;
            skipAhead();
        }
        ImGui::SameLine();
        if (ImGui::Button((">>##scanner_forw_" + name).c_str(), ImVec2(half, 0))) {
            scanUp = true;
            skipAhead();
        }

        // Status: two lines in every state
        if (!running) {
            if (!error.empty()) { ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", error.c_str()); }
            else { ImGui::TextUnformatted("Idle"); }
            ImGui::TextDisabled("%d excluded", (int)lockouts.size());
            return;
        }

        if (receiving) {
            std::string label = bookmarkName(freq);
            if (isPriority(freq)) { label += label.empty() ? "priority" : ", priority"; }
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Receiving %.5f MHz %s", freq / 1e6, label.c_str());
            ImGui::Text("SNR %.1f dB, %s", receivingSNR, (std::fabs(vfoFreq - freq) >= CARRIER_FOLLOW_HZ) ? "off-channel carrier" : "on channel");
            return;
        }

        if (failedRetunes > 0) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Scanning, %d retunes failed", failedRetunes);
        }
        else {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scanning %d channels", (int)channels.size());
        }
        float progress = windows.empty() ? 0.0f : (float)(scanUp ? windowId + 1 : (int)windows.size() - windowId) / (float)windows.size();
        char overlay[64];
        if (lastSweepSeconds > 0.0) { snprintf(overlay, sizeof(overlay), "sweep %.2f s", lastSweepSeconds); }
        else { snprintf(overlay, sizeof(overlay), "first sweep"); }
        ImGui::ProgressBar(progress, ImVec2(menuWidth, 0), overlay);
    }

    // What was received, newest first, also written to scanner_activity.csv
    void drawActivity() {
        if (activity.empty()) { return; }
        std::string header = "Activity (" + std::to_string(activity.size()) + ")##scanner_activity_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) { return; }

        std::string toPlay;
        bool stopPlaying = false;
        for (int i = (int)activity.size() - 1; i >= 0; i--) {
            const Activity& entry = activity[i];
            if (!entry.clip.empty()) {
                bool playing = player.playing() == entry.clip;
                if (ImGui::SmallButton(((playing ? "Stop##scanner_clip_" : "Play##scanner_clip_") + std::to_string(i)).c_str())) {
                    if (playing) { stopPlaying = true; }
                    else { toPlay = entry.clip; }
                }
                ImGui::SameLine();
            }
            ImGui::Text("%s  %.5f MHz  %4.1fs  %4.1f dB  %s", entry.time.c_str(), entry.freq / 1e6, entry.seconds, entry.peakSNR, entry.name.c_str());
        }

        if (stopPlaying) { player.stop(); }
        if (!toPlay.empty()) { player.play(toPlay); }
    }

    // Each transmission to its own file, all of them or only those on chosen frequencies
    void drawRecording() {
        // The header says what is being recorded, ### keeps it the same item when that changes
        const char* summary = (recordMode == RECORD_ALL) ? "every transmission" : (recordMode == RECORD_SELECTED) ? "listed frequencies" : "off";
        std::string header = std::string("Recording: ") + summary + "###scanner_recording_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) { return; }
        float menuWidth = ImGui::GetContentRegionAvail().x;

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Saves each transmission the scanner stops on as its own audio file, play them back under Activity.");
        ImGui::PopTextWrapPos();

        ImGui::LeftLabel("Record");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##scanner_record_mode_" + name).c_str(), &recordMode, "Off\0Every transmission\0Only listed frequencies\0")) {
            updateRecorder();
            saveConfig();
        }
        if (recordMode == RECORD_OFF) { return; }

        if (ImGui::Checkbox(("Also save the raw signal##scanner_record_iq_" + name).c_str(), &saveIq)) {
            updateRecorder();
            saveConfig();
        }
        if (saveIq) { ImGui::TextDisabled("I/Q next to each clip, for tuning the audio offline"); }

        ImGui::LeftLabel("Folder");
        if (recordFolder.render("##scanner_record_folder_" + name) && recordFolder.pathIsValid()) {
            saveConfig();
        }
        if (ImGui::Button(("Open Folder##scanner_record_open_" + name).c_str(), ImVec2(menuWidth, 0))) {
            openFolder(recordFolder.expandString(recordFolder.path));
        }
        if (clipsSaved == 0) { ImGui::TextDisabled("Nothing saved since start"); }
        else { ImGui::TextDisabled("%d saved since start, last %s", clipsSaved, lastClip.c_str()); }

        if (recordMode != RECORD_SELECTED) { return; }
        ImGui::TextUnformatted("Frequencies to record:");
        if (recordFreqs.empty()) { ImGui::TextDisabled("None yet, add them below"); }
        if (drawFrequencyList("record", recordFreqs, recordMHz)) { saveConfig(); }
    }

    // Shows a folder in the system's file manager
    static void openFolder(const std::string& path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
#if defined(_WIN32)
        ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
        const char* opener = "open";
#else
        const char* opener = "xdg-open";
#endif
        // Without a shell, the path is passed as it is whatever it contains
        pid_t pid;
        char* argv[] = { (char*)opener, (char*)path.c_str(), NULL };
#ifdef __APPLE__
        // A plugin can't reach environ directly on macOS
        char** env = *_NSGetEnviron();
#else
        char** env = environ;
#endif
        if (posix_spawnp(&pid, opener, NULL, NULL, argv, env) != 0) {
            flog::error("Scanner: could not open {0}", path);
            return;
        }
        // It hands the folder to the file manager and returns at once
        waitpid(pid, NULL, 0);
#endif
    }

    // Channels that take over from anything else as soon as they are active in the tuning window,
    // and whose window is looked at again every few seconds while scanning
    void drawPriority() {
        std::string header = "Priority (" + std::to_string(priorityFreqs.size()) + ")##scanner_priority_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) { return; }
        if (running) { ImGui::TextDisabled("Changes apply when the scanner starts again"); }
        if (drawFrequencyList("priority", priorityFreqs, priorityMHz)) { saveConfig(); }
    }

    // A list of frequencies with an input to add one and a button to remove each, true if changed
    bool drawFrequencyList(const std::string& id, std::set<long long>& freqs, double& inputMHz) {
        bool changed = false;
        float addWidth = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputDouble(("##scanner_" + id + "_freq_" + name).c_str(), &inputMHz, 0.0, 0.0, "%.5f MHz");
        ImGui::SameLine();
        if (ImGui::Button(("Add##scanner_" + id + "_add_" + name).c_str())) {
            freqs.insert(std::llround(inputMHz * 1e6));
            changed = true;
        }

        long long removed = -1;
        for (long long freq : freqs) {
            std::string label = bookmarkName((double)freq);
            ImGui::Text("%.5f MHz %s", (double)freq / 1e6, label.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(("Remove##scanner_" + id + "_remove_" + std::to_string(freq)).c_str())) {
                removed = freq;
            }
        }
        if (removed >= 0) {
            freqs.erase(removed);
            changed = true;
        }
        return changed;
    }

    void updateRecorder() {
        if (running && recordMode != RECORD_OFF && saveIq) { iqRecorder.enable(); }
        else { iqRecorder.disable(); }
        if (running && recordMode != RECORD_OFF) {
            recorder.attach(vfoName);
            return;
        }
        // Switched off during a transmission: its clip is finished with it, not thrown away
        if (running && state == State::Receiving && recorder.isRecording()) { return; }
        recorder.detach();
    }

    bool isPriority(double freq) {
        return scan::matchesAny(priorityFreqs, freq, 2.0);
    }

    // Name of the frequency manager's bookmark on a frequency, from any of its lists
    std::string bookmarkName(double freq) {
        if (Clock::now() - lastNamesRefresh > std::chrono::seconds(NAMES_REFRESH_S)) { refreshBookmarkNames(); }
        return scan::bookmarkAt(bookmarkNames, freq, BOOKMARK_TOLERANCE_HZ);
    }

    void refreshBookmarkNames() {
        lastNamesRefresh = Clock::now();
        bookmarkNames.clear();
        json fm = readFrequencyManagerConfig();
        if (!fm.contains("lists") || !fm["lists"].is_object()) { return; }
        for (auto& [listName, list] : fm["lists"].items()) {
            if (!list.contains("bookmarks") || !list["bookmarks"].is_object()) { continue; }
            for (auto& [bookmarkName, bookmark] : list["bookmarks"].items()) {
                if (!bookmark.contains("frequency") || !bookmark["frequency"].is_number()) { continue; }
                bookmarkNames.push_back({ bookmark["frequency"].get<double>(), bookmarkName });
            }
        }
    }

    bool recordsChannel(double freq) {
        if (recordMode == RECORD_ALL) { return true; }
        return recordMode == RECORD_SELECTED && scan::matchesAny(recordFreqs, freq, step);
    }

    void startClip(double freq) {
        if (!recordsChannel(freq)) { return; }
        char buf[64];
        std::time_t t = std::chrono::system_clock::to_time_t(receiveWallStart);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
        std::string fileName = std::string(buf);
        snprintf(buf, sizeof(buf), "_%.5fMHz", freq / 1e6);
        fileName += buf;
        std::string label = bookmarkName(freq);
        if (!label.empty()) { fileName += "_" + scan::fileNamePart(label); }

        std::string base = recordFolder.expandString(recordFolder.path) + "/" + fileName;
        std::string path = scan::uniquePath(base, ".wav", [](const std::string& path) { return std::filesystem::exists(path); });
        recorder.start(path);
        // The channel itself, wherever the radio follows a carrier within it
        if (iqRecorder.isEnabled()) { iqRecorder.start(path.substr(0, path.size() - 4) + "_iq.wav", freq - gui::waterfall.getCenterFrequency()); }
    }

    // Ends what was received on the channel: logged and its recording kept only if it was heard
    void endReceive(Clock::time_point now) {
        // Already finished when the user tuned away
        if (tunedAway) { return; }
        std::string clip = recorder.finish(confirmedAfterListen);
        iqRecorder.finish(!clip.empty());
        if (!clip.empty()) {
            clipsSaved++;
            char buf[64];
            std::time_t t = std::chrono::system_clock::to_time_t(receiveWallStart);
            std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " %.5f MHz", receivingFreq / 1e6);
            lastClip = buf;
        }
        if (confirmedAfterListen) {
            // It works, earlier false starts here were something else
            falseStarts.erase(std::llround(receivingFreq));
            logActivity(now, clip);
        }
        if (recordMode == RECORD_OFF) { recorder.detach(); }
    }

    void logActivity(Clock::time_point now, const std::string& clip) {
        Activity entry;
        entry.clip = clip;
        entry.freq = receivingFreq;
        entry.name = bookmarkName(receivingFreq);
        entry.seconds = std::chrono::duration<double>(now - receiveStart).count();
        entry.peakSNR = receivePeakSNR;
        char buf[32];
        std::time_t t = std::chrono::system_clock::to_time_t(receiveWallStart);
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
        entry.time = buf;

        activity.push_back(entry);
        if (activity.size() > MAX_ACTIVITY) { activity.erase(activity.begin()); }

        std::string path = core::args["root"].s() + "/scanner_activity.csv";
        moveOutdatedActivityLog(path);
        bool exists = std::filesystem::exists(path);
        std::ofstream file(path, std::ios::app);
        if (!exists) { file << ACTIVITY_HEADER << "\n"; }
        std::strftime(buf, sizeof(buf), "%Y-%m-%d,%H:%M:%S", std::localtime(&t));
        std::string clipName = clip.empty() ? "" : std::filesystem::path(clip).filename().string();
        file << buf << "," << std::fixed << std::setprecision(5) << entry.freq / 1e6 << "," << csvField(entry.name) << "," << std::setprecision(1) << entry.seconds << "," << entry.peakSNR << "," << csvField(clipName) << "\n";
    }

    static std::string csvField(const std::string& value) {
        if (value.find_first_of(",\"\n") == std::string::npos) { return value; }
        std::string quoted = "\"";
        for (char c : value) {
            quoted += c;
            if (c == '"') { quoted += '"'; }
        }
        return quoted + "\"";
    }

    // A log with other columns is kept aside instead of getting rows that don't match its header
    static void moveOutdatedActivityLog(const std::string& path) {
        std::string header;
        {
            std::ifstream file(path);
            if (!file || !std::getline(file, header) || header == ACTIVITY_HEADER) { return; }
        }
        std::error_code ec;
        std::filesystem::rename(path, path + ".old", ec);
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

    void drawAutoSkips() {
        if (autoSkips.empty()) { return; }
        std::string header = "Auto-skipped (" + std::to_string(autoSkips.size()) + ")##scanner_autoskips_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) { return; }

        long long lockOut = -1;
        long long unskip = -1;
        for (auto& [freq, skip] : autoSkips) {
            ImGui::Text("%.5f MHz, %s", (double)freq / 1e6, skip.reason.c_str());
            if (ImGui::SmallButton(("Lock Out##scanner_auto_lock_" + std::to_string(freq)).c_str())) { lockOut = freq; }
            ImGui::SameLine();
            if (ImGui::SmallButton(("Scan Again##scanner_auto_unskip_" + std::to_string(freq)).c_str())) { unskip = freq; }
        }

        if (lockOut >= 0) {
            lockouts.insert(lockOut);
            autoSkips.erase(lockOut);
            saveConfig();
        }
        if (unskip >= 0) { autoSkips.erase(unskip); }
    }

    void drawExclusions() {
        std::string header = "Excluded (" + std::to_string(lockouts.size()) + ")##scanner_lockouts_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) { return; }

        if (ImGui::Checkbox(("Skip data channels (ACARS, VDL2)##scanner_skip_data_" + name).c_str(), &skipDataChannels)) {
            saveConfig();
        }
        if (skipDataChannels) {
            std::string list;
            for (double freq : scan::dataChannels()) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.3f", freq / 1e6);
                list += (list.empty() ? "" : ", ") + std::string(buf);
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", list.c_str());
            ImGui::PopTextWrapPos();
        }

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

    // Lists of the frequency manager, read from its config file. The first entry, an empty
    // name, follows the list selected in the frequency manager.
    void refreshBookmarkLists() {
        lastListRefresh = Clock::now();
        bookmarkLists.assign(1, "");
        bookmarkListsTxt = "Frequency Manager's list";
        bookmarkListsTxt += '\0';
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
        bookmarkList = bookmarkLists[bookmarkListId];
    }

    static json readFrequencyManagerConfig() {
        std::ifstream file(core::args["root"].s() + "/frequency_manager_config.json");
        json fm = json::parse(file, nullptr, false);
        return fm.is_discarded() ? json::object() : fm;
    }

    std::vector<double> bookmarkChannels() {
        std::vector<double> freqs;
        json fm = readFrequencyManagerConfig();
        scannedList = bookmarkList;
        if (scannedList.empty() && fm.contains("selectedList") && fm["selectedList"].is_string()) { scannedList = fm["selectedList"]; }
        if (!fm.contains("lists") || !fm["lists"].contains(scannedList)) { return freqs; }
        json& bookmarks = fm["lists"][scannedList]["bookmarks"];
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
        float bandwidth = AIRBAND_BANDWIDTH;
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
        // Priority channels are scanned whatever the range or list
        for (long long freq : priorityFreqs) { all.push_back((double)freq); }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end(), [](double a, double b) { return std::fabs(a - b) < 1.0; }), all.end());
        channels.clear();
        for (double freq : all) {
            if (!lockouts.count(std::llround(freq)) && !(skipDataChannels && scan::isDataChannel(freq))) { channels.push_back(freq); }
        }
        if (channels.empty()) {
            error = (mode == MODE_RANGE) ? "Invalid frequency range" : ("No bookmarks in list \"" + scannedList + "\"");
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
        failedRetunes = 0;
        running = true;
        updateRecorder();
        nextWindow();
    }

    void stop(const std::string& reason) {
        if (running && state == State::Receiving) { endReceive(Clock::now()); }
        if (running && userFFTRate < SCAN_FFT_RATE) { sigpath::iqFrontEnd.setFFTRate(userFFTRate); }
        if (running) { setRadioMuted(false); }
        running = false;
        error = reason;
        updateRecorder();
    }

    static void frameEventHandler(bool playing, void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        _this->player.poll();
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

        // The SDR didn't take the last retune: the spectrum still shows the previous window and
        // would be read as this one, stopping on channels where nothing happens
        if (state != State::Receiving && sigpath::sourceManager.getTuneFailures() != tuneFailuresAtTune) {
            failedRetunes++;
            tuneWindow();
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
        detectionFactor = factor;
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

        // Only lock onto channels the radio's squelch would open for, a narrow carrier reaching
        // the trigger often fails its test and the lock would just be a false start
        if (detection.frameCount() >= MIN_AVERAGED_FRAMES) {
            float margin = radioSquelchMargin();
            double bandwidth = radioBandwidth();
            for (int i = 0; i < count; i++) {
                if (snr[i] < trigger) { continue; }
                if (!opensSquelch(window.first + i, margin, bandwidth)) { snr[i] = -INFINITY; }
            }
        }

        // An active priority channel goes first, however strong the others are
        bool priorityActive = false;
        for (int i = 0; i < count; i++) {
            if (snr[i] >= trigger && isPriority(channels[window.first + i])) { priorityActive = true; }
        }
        if (priorityActive) {
            for (int i = 0; i < count; i++) {
                if (!isPriority(channels[window.first + i])) { snr[i] = -INFINITY; }
            }
        }

        // A decision needs at least two frames averaged, one frame is too noisy
        std::vector<int> averagedFrames(count, detection.frameCount());
        while (true) {
            int picked = scan::pickStrongest(snr, averagedFrames, MIN_AVERAGED_FRAMES, trigger);
            if (picked < 0) {
                searchFrames++;
                if (searchFrames >= SEARCH_FRAMES) { nextWindow(); }
                return;
            }

            // Name the channel after where the carrier actually is, which may be a channel that
            // is excluded or skipped
            double center = carrierCenter(window.first + picked);
            int channel = owningChannel(center, window);
            if (isSkipped(channels[channel], now)) {
                snr[picked] = -INFINITY;
                continue;
            }
            listen(channel, snr[picked], center, now);
            return;
        }
    }

    // The channel a carrier belongs to: the nearest one, or on the 8.33kHz grid the 25kHz
    // channel it is an offset carrier of
    int owningChannel(double carrier, const scan::Window& window) {
        int nearest = std::clamp<int>(scan::nearestChannel(channels, carrier), window.first, window.last);
        if (mode != MODE_RANGE) { return nearest; }
        double owner = scan::owningChannel(carrier, channels[nearest], step);
        int channel = scan::nearestChannel(channels, owner);
        return (std::fabs(channels[channel] - owner) < 1.0) ? channel : nearest;
    }

    // Whether the radio's squelch would open for a channel: its test is the power of the whole
    // channel above the noise around it, measured where the radio will listen
    bool opensSquelch(int channel, float margin, double bandwidth) {
        if (margin <= 0.0f) { return true; }
        dsp::noise_reduction::ChannelPower power = dsp::noise_reduction::measureChannel(detectionSpectrum.data, detectionSpectrum.width, detectionSpectrum.start, detectionSpectrum.span, detectionSpectrum.dcFreq, listenFrequency(channel), bandwidth, averagedNoiseSamples());
        return !power.valid || power.channelDb - power.noiseDb >= margin;
    }

    // Independent noise samples behind each detection bin, over bins and frames
    double averagedNoiseSamples() {
        return (double)(detection.frameCount() * detectionFactor) / FFT_BINS_PER_SAMPLE;
    }

    void updateReceiving(Clock::time_point now) {
        // The user tuned away: what was received is finished here, what follows isn't this
        // channel any more. The scanner holds and stays where they put the VFO.
        double actualFreq = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos[vfoName]->centerOffset;
        if (std::fabs(actualFreq - vfoFreq) > 1.0) {
            endReceive(now);
            tunedAway = true;
            hold = true;
            vfoFreq = actualFreq;
        }

        // The radio's squelch decides what is heard, so it also decides how long to stay.
        // Without a level squelch the FFT is used instead.
        // A single frame of noise now and then reaches the threshold and would keep the scanner
        // on an empty channel, so the decision uses a smoothed SNR (averaged as power)
        float frameSNR = channelSNR(receivingChannel, receiveHalfWidth);
        float frameRatio = std::isfinite(frameSNR) ? std::pow(10.0f, frameSNR / 10.0f) : 0.0f;
        smoothedRatio += RECEIVE_SMOOTHING * (frameRatio - smoothedRatio);
        receivingSNR = 10.0f * std::log10(std::max<float>(smoothedRatio, 1e-6f));
        receivePeakSNR = std::max<float>(receivePeakSNR, receivingSNR);
        // Stay only while it can be heard: with a level squelch that is the squelch's call. A
        // signal the FFT sees but the squelch keeps silent held the scanner on a channel playing
        // nothing. Without a squelch the FFT decides.
        int squelch = radioSquelchState();
        bool present = (squelch == RADIO_IFACE_SQUELCH_NONE) ? (receivingSNR >= trigger - RECEIVE_HYSTERESIS) : (squelch == RADIO_IFACE_SQUELCH_OPEN);
        if (present) {
            lastSignal = now;
            confirmedAfterListen = true;
            // Evidence of a reply elsewhere only counts while this channel is quiet
            replyHits.clear();
        }
        else {
            lastAbsent = now;
        }
        if (receivingSNR >= trigger - RECEIVE_HYSTERESIS) {
            modulation.add(fineSpectrum, receivingCarrier());
        }
        if (hold) { return; }

        if (priorityTakesOver(now)) { return; }
        if (!present && watchForReply(now)) { return; }

        // A carrier with nothing beside it is a spur or a stuck transmitter, not a transmission
        if (now - receiveStart >= std::chrono::milliseconds(UNMODULATED_CHECK_MS) && modulation.frameCount() >= MIN_MODULATION_FRAMES && modulation.sidebandToNoiseDb() < UNMODULATED_DB) {
            autoSkip(receivingChannel, "unmodulated carrier", now);
            return;
        }

        // ATC talks in short transmissions, something that never pauses is a broadcast like ATIS
        if (now - lastAbsent >= std::chrono::seconds(CONTINUOUS_LIMIT_S)) {
            autoSkip(receivingChannel, "continuous signal", now);
            return;
        }

        // Nothing in the frames after locking on: it was a noise spike or a stale frame from before
        // the retune, don't wait for the linger time
        framesSinceListen++;
        if (!confirmedAfterListen && now - receiveStart >= std::chrono::milliseconds(VERIFY_MS)) {
            falseStart(receivingChannel, now);
            return;
        }

        // The aircraft and the ground station take turns and are rarely on the same frequency,
        // whoever is talking now is followed. This frame decides, the smoothed SNR still holds
        // up in the pause after an over, when the strongest point in the channel is noise.
        if (frameSNR >= trigger) {
            followCarrier(receivingCarrier());
        }
        else {
            // A jump needs two frames in a row, not two with a gap between them
            pendingJump = NAN;
        }

        // Once the conversation is over the sweep goes on, a busy channel can't keep the rest of
        // the band from being heard. Its window comes round again within a sweep.
        if (maxListenTime > 0 && now - receiveStart > std::chrono::seconds(maxListenTime)) {
            tempSkips[std::llround(receivingFreq)] = now + std::chrono::seconds(SKIP_TIME_S);
            resumeSearching();
            nextWindow();
            return;
        }

        if (now - lastSignal > std::chrono::milliseconds(lingerTime)) {
            resumeSearching();
            nextWindow();
        }
    }

    // A priority channel that becomes active in the tuning window takes over, also in the middle
    // of a transmission elsewhere. It must pass the same test as a lock, twice in a row.
    bool priorityTakesOver(Clock::time_point now) {
        if (priorityFreqs.empty() || isPriority(receivingFreq)) { return false; }
        const scan::Window& window = windows[windowId];
        float margin = radioSquelchMargin();
        double bandwidth = radioBandwidth();

        int best = -1;
        float bestSNR = -INFINITY;
        for (int i = window.first; i <= window.last; i++) {
            if (!isPriority(channels[i])) { continue; }
            float snr = isSkipped(channels[i], now) ? -INFINITY : channelSNR(i);
            bool passes = snr >= trigger && opensSquelch(i, margin, bandwidth);
            priorityHits[i] = passes ? priorityHits[i] + 1 : 0;
            if (priorityHits[i] >= MIN_AVERAGED_FRAMES && snr > bestSNR) {
                best = i;
                bestSNR = snr;
            }
        }
        if (best < 0) { return false; }

        flog::info("Scanner: priority {0:.5f} MHz takes over from {1:.5f} MHz", channels[best] / 1e6, receivingFreq / 1e6);
        endReceive(now);
        listen(best, bestSNR, carrierCenter(best), now);
        return true;
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

    // Auto squelch margin in dB, 0 when the radio isn't on auto squelch
    float radioSquelchMargin() {
        float margin = 0.0f;
        if (radioAvailable()) { core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_SQUELCH_MARGIN, NULL, &margin); }
        return margin;
    }

    double radioBandwidth() {
        float bandwidth = (float)(step * 0.84);
        if (radioAvailable()) { core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &bandwidth); }
        return bandwidth;
    }

    int radioSquelchState() {
        int state = RADIO_IFACE_SQUELCH_NONE;
        if (radioAvailable()) {
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_SQUELCH_STATE, NULL, &state);
        }
        return state;
    }

    // While the channel is quiet, the other side of the conversation may answer a little off it:
    // aircraft or ground stations off frequency, offset carriers. Channels around it are watched
    // and the scanner moves there when one passes the same test as a normal lock twice in a row.
    bool watchForReply(Clock::time_point now) {
        // Only once something was heard here, a lock that never opened the squelch isn't a conversation
        if (replyWatchKHz <= 0.0f || !confirmedAfterListen) { return false; }
        // Following the carrier closed the squelch for a moment, the transmission goes on
        if (now - lastVfoTune < std::chrono::milliseconds(SQUELCH_SETTLE_MS)) { return false; }
        const scan::Window& window = windows[windowId];
        double here = receivingFreq;
        float margin = radioSquelchMargin();
        double bandwidth = radioBandwidth();

        int best = -1;
        float bestSNR = -INFINITY;
        for (int i = window.first; i <= window.last; i++) {
            double offset = std::fabs(channels[i] - here);
            if (i == receivingChannel || offset > replyWatchKHz * 1000.0) { continue; }
            float snr = isSkipped(channels[i], now) ? -INFINITY : channelSNR(i);
            bool passes = snr >= trigger && opensSquelch(i, margin, bandwidth);
            replyHits[i] = passes ? replyHits[i] + 1 : 0;
            if (replyHits[i] >= MIN_AVERAGED_FRAMES && snr > bestSNR) {
                best = i;
                bestSNR = snr;
            }
        }
        if (best < 0) { return false; }

        flog::info("Scanner: reply on {0:.5f} MHz next to {1:.5f} MHz", channels[best] / 1e6, here / 1e6);
        endReceive(now);
        replyHits.clear();
        listen(best, bestSNR, carrierCenter(best), now);
        return true;
    }

    double channelHalfWidth() {
        return scan::channelHalfWidth(step);
    }

    double dcGuard() {
        return detectionSpectrum.binWidth() * 1.5;
    }

    float channelSNR(int channel) {
        return channelSNR(channel, channelHalfWidth());
    }

    float channelSNR(int channel, double halfWidth) {
        double freq = channels[channel];
        float peak = scan::channelPeak(detectionSpectrum, freq, halfWidth, dcGuard());
        float floor = scan::noiseFloor(detectionSpectrum, freq, FLOOR_RADIUS, halfWidth, dcGuard(), FLOOR_QUANTILE);
        return peak - floor;
    }

    // Centered on the full resolution FFT, a carrier is located to within a few Hz
    double carrierCenter(int channel) {
        return scan::signalCenter(fineSpectrum, channels[channel], channelHalfWidth(), dcGuard());
    }

    // Carrier of whoever is talking on the channel being received
    double receivingCarrier() {
        return scan::signalCenter(fineSpectrum, receivingFreq, receiveHalfWidth, dcGuard());
    }

    // Where to listen for what is on a channel: on its carrier, the filter is only as wide as
    // the voice. Ground stations on offset carrier systems are several kHz off, aircraft a few
    // hundred Hz to over a kHz.
    double listenFrequency(int channel) {
        double carrier = carrierCenter(channel);
        return (std::fabs(carrier - channels[channel]) > CARRIER_FOLLOW_HZ) ? carrier : channels[channel];
    }

    // Keeps the VFO on the carrier of whoever is talking. Every retune briefly resets the radio's
    // squelch, so the VFO only moves once it is clearly off, drift is smoothed, and moving to
    // another transmitter is a single jump that waits for the next frame to agree, one frame of
    // noise elsewhere isn't a transmitter.
    void followCarrier(double carrier) {
        if (std::fabs(carrier - trackedCenter) <= JUMP_HZ) {
            pendingJump = NAN;
            trackedCenter += CENTER_SMOOTHING * (carrier - trackedCenter);
        }
        else {
            bool confirmed = std::isfinite(pendingJump) && std::fabs(carrier - pendingJump) <= JUMP_HZ;
            pendingJump = carrier;
            if (!confirmed) { return; }
            pendingJump = NAN;
            trackedCenter = carrier;
        }

        if (std::fabs(trackedCenter - vfoFreq) < CARRIER_FOLLOW_HZ) { return; }
        tuneVFO(trackedCenter);
    }

    // Something the FFT sees but the squelch never lets through, like a spur or a weak carrier,
    // would stop the scanner on every sweep. Once it is skipped for a moment, twice for longer.
    void falseStart(int channel, Clock::time_point now) {
        long long key = std::llround(channels[channel]);
        auto it = falseStarts.find(key);
        bool repeated = (it != falseStarts.end()) && (now - it->second < std::chrono::minutes(FALSE_START_MEMORY_MINUTES));
        falseStarts[key] = now;
        if (repeated) {
            falseStarts.erase(key);
            autoSkip(channel, "nothing audible", now);
            return;
        }
        tempSkips[key] = now + std::chrono::seconds(FALSE_START_SKIP_S);
        resumeSearching();
    }

    void autoSkip(int channel, const std::string& reason, Clock::time_point now) {
        long long key = std::llround(channels[channel]);
        flog::info("Scanner: skipping {0:.5f} MHz, {1}", channels[channel] / 1e6, reason);
        Strikes& strikes = autoSkipStrikes[key];
        if (now - strikes.last > std::chrono::minutes(STRIKE_MEMORY_MINUTES)) { strikes.count = 0; }
        int minutes = scan::autoSkipMinutes(strikes.count, isPriority(channels[channel]));
        strikes.count++;
        strikes.last = now;
        autoSkips[key] = { now + std::chrono::minutes(minutes), reason };
        resumeSearching();
    }

    bool isSkipped(double freq, Clock::time_point now) {
        long long key = std::llround(freq);
        if (lockouts.count(key)) { return true; }
        if (skipDataChannels && scan::isDataChannel(freq)) { return true; }

        auto autoIt = autoSkips.find(key);
        if (autoIt != autoSkips.end()) {
            if (autoIt->second.until > now) { return true; }
            autoSkips.erase(autoIt);
        }

        auto it = tempSkips.find(key);
        if (it == tempSkips.end()) { return false; }
        if (it->second > now) { return true; }

        tempSkips.erase(it);
        return false;
    }

    void listen(int channel, float snr, double center, Clock::time_point now) {
        receiveWallStart = std::chrono::system_clock::now();
        receivePeakSNR = snr;
        smoothedRatio = std::pow(10.0f, snr / 10.0f);
        flog::info("Scanner: receiving {0:.5f} MHz, carrier at {1:.5f} MHz, {2:.1f} dB", channels[channel] / 1e6, center / 1e6, snr);
        state = State::Receiving;
        receivingChannel = channel;
        receivingFreq = channels[channel];
        // A 25kHz channel heard on an offset carrier outside its own 8.33kHz cell: its other
        // carriers and the aircraft on the channel itself are looked for within the offset reach
        bool offsetCarrier = std::fabs(center - receivingFreq) > channelHalfWidth();
        receiveHalfWidth = scan::carrierSearchHalfWidth(offsetCarrier, step);
        tunedAway = false;
        receivingSNR = snr;
        receiveStart = now;
        lastSignal = now;
        hold = false;
        framesSinceListen = 0;
        confirmedAfterListen = false;

        trackedCenter = (std::fabs(center - receivingFreq) > CARRIER_FOLLOW_HZ) ? center : receivingFreq;
        pendingJump = NAN;
        replyHits.clear();
        priorityHits.clear();
        modulation.reset();
        lastAbsent = now;
        tuneVFO(trackedCenter);
        setRadioMuted(false);
        startClip(receivingFreq);
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
        if (state == State::Receiving) {
            setRadioMuted(true);
            endReceive(Clock::now());
        }
        state = State::Searching;
        searchFrames = 0;
        detection.reset();
    }

    void nextWindow() {
        auto now = Clock::now();
        if (windowId >= 0 && windowId < (int)windowSearched.size()) { windowSearched[windowId] = now; }
        if (gui::waterfall.getBandwidth() != plannedBandwidth) {
            planWindows();
        }

        // Back from looking at a priority window, the sweep goes on where it was
        if (detourFrom >= 0 && detourFrom < (int)windows.size()) { windowId = detourFrom; }
        detourFrom = -1;

        // A priority channel's window not looked at for a while comes first
        for (int w : priorityWindows) {
            if (w == windowId || now - windowSearched[w] < std::chrono::milliseconds(PRIORITY_REVISIT_MS)) { continue; }
            detourFrom = windowId;
            windowId = w;
            tuneWindow();
            return;
        }

        int count = (int)windows.size();
        int next = ((windowId + (scanUp ? 1 : -1)) % count + count) % count;
        bool wrapped = scanUp ? (next == 0) : (next == count - 1);
        if (wrapped && windowId >= 0) {
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
        double dcClearance = (mode == MODE_RANGE) ? step / 2.0 : channelHalfWidth() + DC_CLEARANCE_MARGIN;
        windows = scan::planWindows(channels, plannedBandwidth * USABLE_BANDWIDTH, dcClearance);

        windowSearched.assign(windows.size(), Clock::now());
        priorityWindows.clear();
        detourFrom = -1;
        for (int w = 0; w < (int)windows.size(); w++) {
            for (int i = windows[w].first; i <= windows[w].last; i++) {
                if (!isPriority(channels[i])) { continue; }
                priorityWindows.push_back(w);
                break;
            }
        }
    }

    void tuneWindow() {
        // The SDR moved or its sample rate changed during a reception: it is finished properly
        if (state == State::Receiving) { enterSearching(); }
        const scan::Window& window = windows[windowId];
        // The main window tunes the SDR to the moved center later in this same frame (the scanner
        // runs at its start), tuning here as well retuned the hardware twice per window
        gui::waterfall.setCenterFrequency(window.center);
        gui::waterfall.centerFreqMoved = true;
        tuneFailuresAtTune = sigpath::sourceManager.getTuneFailures();
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
        lastVfoTune = Clock::now();
        sigpath::vfoManager.setCenterOffset(vfoName, freq - gui::waterfall.getCenterFrequency());
    }

    void loadConfig() {
        config.acquire();
        json& conf = config.conf;
        if (conf.contains("startFreq")) { startFreq = conf["startFreq"]; }
        if (conf.contains("stopFreq")) { stopFreq = conf["stopFreq"]; }
        if (conf.contains("step")) { setStep(conf["step"]); }
        if (conf.contains("trigger")) { trigger = conf["trigger"]; }
        if (conf.contains("tuningTime")) { tuningTime = conf["tuningTime"]; }
        if (conf.contains("lingerTime")) { lingerTime = conf["lingerTime"]; }
        if (conf.contains("maxListenTime")) { maxListenTime = conf["maxListenTime"]; }
        if (conf.contains("replyWatch")) { replyWatchKHz = conf["replyWatch"]; }
        if (conf.contains("mode")) { mode = conf["mode"]; }
        if (conf.contains("bookmarkList")) { bookmarkList = conf["bookmarkList"]; }
        if (conf.contains("recordMode")) { recordMode = conf["recordMode"]; }
        if (conf.contains("saveIq")) { saveIq = conf["saveIq"]; }
        if (conf.contains("skipDataChannels")) { skipDataChannels = conf["skipDataChannels"]; }
        if (conf.contains("priorityFrequencies")) {
            priorityFreqs.clear();
            for (long long freq : conf["priorityFrequencies"]) {
                priorityFreqs.insert(freq);
            }
        }
        if (conf.contains("recordFolder")) { recordFolder.setPath(conf["recordFolder"]); }
        if (conf.contains("recordFrequencies")) {
            recordFreqs.clear();
            for (long long freq : conf["recordFrequencies"]) {
                recordFreqs.insert(freq);
            }
        }
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
        conf["tuningTime"] = tuningTime;
        conf["lingerTime"] = lingerTime;
        conf["maxListenTime"] = maxListenTime;
        conf["replyWatch"] = replyWatchKHz;
        conf["mode"] = mode;
        conf["bookmarkList"] = bookmarkList;
        conf["recordMode"] = recordMode;
        conf["saveIq"] = saveIq;
        conf["skipDataChannels"] = skipDataChannels;
        conf["priorityFrequencies"] = priorityFreqs;
        conf["recordFolder"] = recordFolder.path;
        conf["recordFrequencies"] = recordFreqs;
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

    static constexpr int RECORD_OFF = 0;
    static constexpr int RECORD_ALL = 1;
    static constexpr int RECORD_SELECTED = 2;
    static constexpr const char* DEFAULT_RECORD_FOLDER = "%ROOT%/recordings/scanner";
    static constexpr const char* ACTIVITY_HEADER = "date,time,frequency_mhz,name,duration_s,peak_snr_db,recording";

    // Every new FFT frame is evaluated, the tick only bounds how often the FFT is polled
    static constexpr int TICK_MS = 5;
    static constexpr int SEARCH_FRAMES = 3;
    // Time for the squelch to open after locking on, else it was a false start
    static constexpr int VERIFY_MS = 250;
    static constexpr int SCAN_FFT_RATE = 60;
    static constexpr double DC_CLEARANCE_MARGIN = 2000.0;
    static constexpr int MIN_AVERAGED_FRAMES = 2;
    static constexpr uint64_t FRAMES_AFTER_TUNE = 3;
    static constexpr int SKIP_TIME_S = 30;
    static constexpr float RECEIVE_HYSTERESIS = 3.0f;
    static constexpr float RECEIVE_SMOOTHING = 0.2f;
    // Per frame, about 170ms at the scanner's 60 frames per second: the carrier estimate of a
    // weak signal wobbles from frame to frame, the VFO shouldn't follow the wobble
    static constexpr double CENTER_SMOOTHING = 0.1;
    // How far the VFO may be off the carrier before it is moved, the 8kHz filter leaves room
    // for this and the voice still fits
    static constexpr double CARRIER_FOLLOW_HZ = 300.0;
    // A move larger than this is another transmitter, not drift
    static constexpr double JUMP_HZ = 500.0;
    // As the radio's AM default: the whole voice, a neighbouring 8.33kHz channel's voice only
    // begins 5.3kHz away
    static constexpr float AIRBAND_BANDWIDTH = 8000.0f;
    static constexpr int UNMODULATED_CHECK_MS = 4000;
    static constexpr int MIN_MODULATION_FRAMES = 30;
    static constexpr float UNMODULATED_DB = 0.3f;
    static constexpr int CONTINUOUS_LIMIT_S = 30;
    // Auto skips grow longer when a channel keeps getting skipped within this time
    static constexpr int STRIKE_MEMORY_MINUTES = 60;
    // A priority channel's window is looked at again after this, before the sweep goes on
    static constexpr int PRIORITY_REVISIT_MS = 3000;
    // The radio's squelch resets when the VFO moves, a closed squelch right after isn't the end
    // of a transmission
    static constexpr int SQUELCH_SETTLE_MS = 300;
    // Adjacent FFT bins overlap, averaging a group of them is worth about half as many
    // independent noise samples with the Nuttall window used by default
    static constexpr double FFT_BINS_PER_SAMPLE = 2.0;
    static constexpr double BOOKMARK_TOLERANCE_HZ = 500.0;
    static constexpr int NAMES_REFRESH_S = 5;
    static constexpr int FALSE_START_SKIP_S = 30;
    static constexpr int FALSE_START_MEMORY_MINUTES = 5;

    // Only the flat middle of the spectrum is used, the edges roll off
    static constexpr double USABLE_BANDWIDTH = 0.85;
    static constexpr double FLOOR_RADIUS = 150000.0;
    static constexpr float FLOOR_QUANTILE = 0.3f;

    std::string name;
    bool enabled = true;
    EventHandler<bool> frameHandler;

    double startFreq = 118000000.0;
    double stopFreq = 136991667.0;
    double step = AIRBAND_833;
    float trigger = 8.0f;
    int tuningTime = 30;
    int lingerTime = 3000;
    int maxListenTime = 120;
    float replyWatchKHz = 10.0f;
    std::map<int, int> replyHits;
    std::set<long long> lockouts;
    bool skipDataChannels = true;
    std::map<long long, Clock::time_point> tempSkips;

    struct Activity {
        std::string time;
        double freq;
        std::string name;
        double seconds;
        float peakSNR;
        std::string clip;
    };
    std::vector<Activity> activity;
    static constexpr size_t MAX_ACTIVITY = 50;
    std::chrono::system_clock::time_point receiveWallStart;
    float receivePeakSNR = 0.0f;
    float smoothedRatio = 1.0f;

    struct AutoSkip {
        Clock::time_point until;
        std::string reason;
    };
    std::map<long long, AutoSkip> autoSkips;
    std::map<long long, Clock::time_point> falseStarts;
    struct Strikes {
        int count = 0;
        Clock::time_point last;
    };
    std::map<long long, Strikes> autoSkipStrikes;

    // Guard: checked whenever it is in the tuning window, takes over from anything else
    std::set<long long> priorityFreqs = { 121500000 };
    double priorityMHz = 121.5;
    std::map<int, int> priorityHits;
    std::vector<int> priorityWindows;
    std::vector<Clock::time_point> windowSearched;
    int detourFrom = -1;

    std::vector<std::pair<double, std::string>> bookmarkNames;
    Clock::time_point lastNamesRefresh;

    // The guard frequency, whatever is said there is worth keeping
    int recordMode = RECORD_SELECTED;
    std::set<long long> recordFreqs = { 121500000 };
    double recordMHz = 121.5;
    FolderSelect recordFolder;
    bool saveIq = false;
    IqRecorder iqRecorder;
    int clipsSaved = 0;
    std::string lastClip;
    TransmissionRecorder recorder;
    ClipPlayer player;

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
    std::string scannedList;
    Clock::time_point lastListRefresh;
    static constexpr int LIST_REFRESH_S = 2;
    double excludeMHz = 121.5;

    std::vector<double> channels;
    int userFFTRate = 20;
    Clock::time_point sweepStart;
    double lastSweepSeconds = 0.0;
    uint64_t tuneFailuresAtTune = 0;
    int failedRetunes = 0;
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
    // What the reception is named after, logged and recorded as, and how far around it the
    // carrier is looked for: wider for a 25kHz channel heard on an offset carrier
    double receivingFreq = 0.0;
    double receiveHalfWidth = 0.0;
    bool tunedAway = false;
    Clock::time_point lastVfoTune;
    int detectionFactor = 1;
    int framesSinceListen = 0;
    bool confirmedAfterListen = false;
    float receivingSNR = 0.0f;
    double vfoFreq = 0.0;
    double trackedCenter = 0.0;
    double pendingJump = NAN;
    scan::ModulationMeter modulation;
    Clock::time_point lastAbsent;
    Clock::time_point receiveStart;
    Clock::time_point lastSignal;
};

MOD_EXPORT void _INIT_() {
    std::error_code ec;
    std::filesystem::create_directories(core::args["root"].s() + "/recordings/scanner", ec);
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
