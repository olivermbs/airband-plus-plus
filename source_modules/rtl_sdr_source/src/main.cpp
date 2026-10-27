#include <utils/flog.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/smgui.h>
#include <rtl-sdr.h>
#include "gain_finder.h"

#ifdef __ANDROID__
#include <android_backend.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rtl_sdr_source",
    /* Description:     */ "RTL-SDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

const double sampleRates[] = {
    250000,
    1024000,
    1536000,
    1792000,
    1920000,
    2048000,
    2160000,
    2400000,
    2560000,
    2880000,
    3200000
};

const char* sampleRatesTxt[] = {
    "250KHz",
    "1.024MHz",
    "1.536MHz",
    "1.792MHz",
    "1.92MHz",
    "2.048MHz",
    "2.16MHz",
    "2.4MHz",
    "2.56MHz",
    "2.88MHz",
    "3.2MHz"
};

const char* directSamplingModesTxt = "Disabled\0I branch\0Q branch\0";

class RTLSDRSourceModule : public ModuleManager::Instance {
public:
    RTLSDRSourceModule(std::string name) {
        this->name = name;

        serverMode = (bool)core::args["server"];

        sampleRate = sampleRates[0];

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.asyncTuning = true;
        handler.stream = &stream;

        strcpy(dbTxt, "--");

        for (int i = 0; i < 11; i++) {
            sampleRateListTxt += sampleRatesTxt[i];
            sampleRateListTxt += '\0';
        }

        refresh();

        config.acquire();
        if (!config.conf["device"].is_string()) {
            selectedDevName = "";
            config.conf["device"] = "";
        }
        else {
            selectedDevName = config.conf["device"];
        }
        config.release(true);
        selectByName(selectedDevName);

        sigpath::sourceManager.registerSource("RTL-SDR", &handler);

        // Recovery from a lost device needs the GUI, there is no main window in server mode
        if (!serverMode) {
            frameHandler.handler = frameEventHandler;
            frameHandler.ctx = this;
            gui::mainWindow.onFrame.bindHandler(&frameHandler);
        }
    }

    ~RTLSDRSourceModule() {
        if (!serverMode) { gui::mainWindow.onFrame.unbindHandler(&frameHandler); }
        stop(this);
        sigpath::sourceManager.unregisterSource("RTL-SDR");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {
        devNames.clear();
        devListTxt = "";

#ifndef __ANDROID__
        devCount = rtlsdr_get_device_count();
        char buf[1024];
        char venBuf[256];
        char prodBuf[256];
        char snBuf[256];
        for (int i = 0; i < devCount; i++) {
            // Gather device info
            const char* devName = rtlsdr_get_device_name(i);
            int snErr = rtlsdr_get_device_usb_strings(i, venBuf, prodBuf, snBuf);

            // Build name
            if (venBuf[0] && prodBuf[0]) {
                sprintf(buf, "%s %s [%s]##%d", venBuf, prodBuf, (!snErr && snBuf[0]) ? snBuf : "No Serial", i);
            }
            else {
                sprintf(buf, "%s [%s]##%d", devName, (!snErr && snBuf[0]) ? snBuf : "No Serial", i);
            }

            // Add device to list
            devNames.push_back(buf);
            devListTxt += buf;
            devListTxt += '\0';
        }
#else
        // Check for device connection
        devCount = 0;
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::RTL_SDR_VIDPIDS);
        if (devFd < 0) { return; }

        // Generate fake device info
        devCount = 1;
        std::string fakeName = "RTL-SDR Dongle USB";
        devNames.push_back(fakeName);
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    // Device names end with "##<index>", which can change when the device is plugged back in
    static std::string withoutIndex(const std::string& devName) {
        return devName.substr(0, devName.find("##"));
    }

    int findDevice(const std::string& devName) {
        for (int i = 0; i < devCount; i++) {
            if (withoutIndex(devNames[i]) == withoutIndex(devName)) { return i; }
        }
        return -1;
    }

    void selectFirst() {
        if (devCount > 0) {
            selectById(0);
        }
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devCount; i++) {
            if (name == devNames[i]) {
                selectById(i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        selectedDevName = devNames[id];

#ifndef __ANDROID__
        int oret = rtlsdr_open(&openDev, id);
#else
        int oret = rtlsdr_open_sys_dev(&openDev, devFd);
#endif
        
        if (oret < 0) {
            selectedDevName = "";
            flog::error("Could not open RTL-SDR: {0}", oret);
            return;
        }

        gainList.clear();
        int gains[256];
        int n = rtlsdr_get_tuner_gains(openDev, gains);
        gainList = std::vector<int>(gains, gains + n);
        std::sort(gainList.begin(), gainList.end());

        bool created = false;
        config.acquire();
        if (!config.conf["devices"].contains(selectedDevName)) {
            created = true;

            // Highest sample rate without dropped samples, and the gain where the SNR stopped
            // improving on airband with an RTL-SDR Blog V4
            gainId = 0;
            for (int i = 0; i < (int)gainList.size(); i++) {
                if (std::abs(gainList[i] - DEFAULT_GAIN) < std::abs(gainList[gainId] - DEFAULT_GAIN)) { gainId = i; }
            }
            config.conf["devices"][selectedDevName]["sampleRate"] = 2560000.0;
            config.conf["devices"][selectedDevName]["directSampling"] = directSamplingMode;
            config.conf["devices"][selectedDevName]["ppm"] = 0;
            config.conf["devices"][selectedDevName]["biasT"] = biasT;
            config.conf["devices"][selectedDevName]["offsetTuning"] = offsetTuning;
            config.conf["devices"][selectedDevName]["rtlAgc"] = rtlAgc;
            config.conf["devices"][selectedDevName]["tunerAgc"] = tunerAgc;
            config.conf["devices"][selectedDevName]["gain"] = gainId;
        }

        // Load config
        if (config.conf["devices"][selectedDevName].contains("sampleRate")) {
            int selectedSr = config.conf["devices"][selectedDevName]["sampleRate"];
            for (int i = 0; i < 11; i++) {
                if (sampleRates[i] == selectedSr) {
                    srId = i;
                    sampleRate = selectedSr;
                    break;
                }
            }
        }

        if (config.conf["devices"][selectedDevName].contains("directSampling")) {
            directSamplingMode = config.conf["devices"][selectedDevName]["directSampling"];
        }

        if (config.conf["devices"][selectedDevName].contains("ppm")) {
            ppm = config.conf["devices"][selectedDevName]["ppm"];
        }

        if (config.conf["devices"][selectedDevName].contains("biasT")) {
            biasT = config.conf["devices"][selectedDevName]["biasT"];
        }

        if (config.conf["devices"][selectedDevName].contains("offsetTuning")) {
            offsetTuning = config.conf["devices"][selectedDevName]["offsetTuning"];
        }

        if (config.conf["devices"][selectedDevName].contains("rtlAgc")) {
            rtlAgc = config.conf["devices"][selectedDevName]["rtlAgc"];
        }

        if (config.conf["devices"][selectedDevName].contains("tunerAgc")) {
            tunerAgc = config.conf["devices"][selectedDevName]["tunerAgc"];
        }

        if (config.conf["devices"][selectedDevName].contains("gain")) {
            gainId = config.conf["devices"][selectedDevName]["gain"];
        }

        if (gainId >= gainList.size()) { gainId = gainList.size() - 1; }
        updateGainTxt();

        config.release(created);

        rtlsdr_close(openDev);
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        _this->selected = true;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("RTLSDRSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        _this->selected = false;
        _this->waitingForDevice = false;
        flog::info("RTLSDRSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        if (_this->running) { return; }
        _this->waitingForDevice = false;
        if (_this->selectedDevName == "") {
            flog::error("No device selected");
            return;
        }

#ifndef __ANDROID__
        int oret = rtlsdr_open(&_this->openDev, _this->devId);
#else
        int oret = rtlsdr_open_sys_dev(&_this->openDev, _this->devFd);
#endif

        if (oret < 0) {
            flog::error("Could not open RTL-SDR");
            return;
        }

        flog::info("RTL-SDR Sample Rate: {0}", _this->sampleRate);

        rtlsdr_set_sample_rate(_this->openDev, _this->sampleRate);
        rtlsdr_set_center_freq(_this->openDev, _this->freq);
        rtlsdr_set_freq_correction(_this->openDev, _this->ppm);
        rtlsdr_set_tuner_bandwidth(_this->openDev, 0);
        rtlsdr_set_direct_sampling(_this->openDev, _this->directSamplingMode);
        rtlsdr_set_bias_tee(_this->openDev, _this->biasT);
        rtlsdr_set_agc_mode(_this->openDev, _this->rtlAgc);
        rtlsdr_set_tuner_gain(_this->openDev, _this->gainList[_this->gainId]);
        if (_this->tunerAgc) {
            rtlsdr_set_tuner_gain_mode(_this->openDev, 0);
        }
        else {
            rtlsdr_set_tuner_gain_mode(_this->openDev, 1);
            rtlsdr_set_tuner_gain(_this->openDev, _this->gainList[_this->gainId]);
        }
        rtlsdr_set_offset_tuning(_this->openDev, _this->offsetTuning);

        // Bytes per USB transfer, two per sample, a multiple of 512
        _this->asyncCount = std::max<int>(1, (int)roundf(_this->sampleRate * 2.0 * USB_TRANSFER_SECONDS / 512.0)) * 512;

        _this->deviceLost = false;
        _this->workerFinished = false;
        _this->running = true;
        _this->workerThread = std::thread(&RTLSDRSourceModule::worker, _this);

        _this->tunePending = false;
        _this->tunerRunning = true;
        _this->tunerThread = std::thread(&RTLSDRSourceModule::tunerWorker, _this);

        flog::info("RTLSDRSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        if (!_this->running) { return; }

        {
            std::lock_guard<std::mutex> lck(_this->tuneMtx);
            _this->tunerRunning = false;
        }
        _this->tuneCnd.notify_all();
        if (_this->tunerThread.joinable()) { _this->tunerThread.join(); }

        _this->running = false;
        _this->stream.stopWriter();

        // A cancel before the worker started streaming is ignored by librtlsdr, so repeat it until the worker is done
        while (!_this->workerFinished) {
            rtlsdr_cancel_async(_this->openDev);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (_this->workerThread.joinable()) { _this->workerThread.join(); }
        _this->stream.clearWriteStop();
        rtlsdr_close(_this->openDev);
        flog::info("RTLSDRSourceModule '{0}': Stop!", _this->name);
    }

    // Retuning takes 20-30ms of USB transfers. It's done on its own thread so the caller,
    // usually the GUI, never waits for it. Requests made meanwhile collapse into the newest one.
    static void tune(double freq, void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        _this->freq = freq;
        uint64_t request = sigpath::sourceManager.getTuneRequests();
        if (_this->running) {
            {
                std::lock_guard<std::mutex> lck(_this->tuneMtx);
                _this->pendingFreq = freq;
                _this->pendingRequest = request;
                _this->tunePending = true;
            }
            _this->tuneCnd.notify_one();
        }
        else {
            // The frequency is set when it starts
            sigpath::sourceManager.reportTuned(request);
        }
        flog::info("RTLSDRSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    void tunerWorker() {
        while (true) {
            double target;
            uint64_t request;
            {
                std::unique_lock<std::mutex> lck(tuneMtx);
                tuneCnd.wait(lck, [this]() { return tunePending || !tunerRunning; });
                if (!tunerRunning) { return; }
                target = pendingFreq;
                request = pendingRequest;
                tunePending = false;
            }

            std::lock_guard<std::mutex> devLck(devMtx);
            uint32_t newFreq = target;
            bool tuned = false;
            for (int i = 0; i < TUNE_ATTEMPTS && !tuned; i++) {
                // Give the tuner's I2C bus a moment instead of hammering it
                if (i > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(TUNE_RETRY_MS)); }
                tuned = (rtlsdr_set_center_freq(openDev, newFreq) == 0) && (rtlsdr_get_center_freq(openDev) == newFreq);
            }

            if (tuned) {
                failedTunes = 0;
                sigpath::sourceManager.reportTuned(request);
                continue;
            }

            flog::warn("RTL-SDR failed to tune to {0} Hz", newFreq);
            sigpath::sourceManager.reportTuneFailed();

            // A tuner that stopped answering is reset by reopening the device, like after it was lost
            if (++failedTunes >= MAX_FAILED_TUNES) {
                flog::error("RTL-SDR tuner stopped responding, restarting the device");
                failedTunes = 0;
                deviceLost = true;
            }
        }
    }

    static void menuHandler(void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_rtlsdr_dev_sel_", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectById(_this->devId);
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["device"] = _this->selectedDevName;
                config.release(true);
            }
        }

        if (SmGui::Combo(CONCAT("##_rtlsdr_sr_sel_", _this->name), &_this->srId, _this->sampleRateListTxt.c_str())) {
            _this->sampleRate = sampleRates[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["sampleRate"] = _this->sampleRate;
                config.release(true);
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_rtlsdr_refr_", _this->name)/*, ImVec2(refreshBtnWdith, 0)*/)) {
            _this->refresh();
            _this->selectByName(_this->selectedDevName);
            core::setInputSampleRate(_this->sampleRate);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        if (_this->waitingForDevice) {
            SmGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Device lost, waiting for it to come back...");
            if (SmGui::Button(CONCAT("Stop Waiting##_rtlsdr_stop_waiting_", _this->name))) {
                _this->waitingForDevice = false;
            }
        }

        // Rest of rtlsdr config here
        SmGui::LeftLabel("Direct Sampling");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtlsdr_ds_", _this->name), &_this->directSamplingMode, directSamplingModesTxt)) {
            if (_this->running) {
                std::lock_guard<std::mutex> devLck(_this->devMtx);
                rtlsdr_set_direct_sampling(_this->openDev, _this->directSamplingMode);

                // Update gains (fix for librtlsdr bug)
                if (_this->directSamplingMode == false) {
                    rtlsdr_set_agc_mode(_this->openDev, _this->rtlAgc);
                    if (_this->tunerAgc) {
                        rtlsdr_set_tuner_gain_mode(_this->openDev, 0);
                    }
                    else {
                        rtlsdr_set_tuner_gain_mode(_this->openDev, 1);
                        rtlsdr_set_tuner_gain(_this->openDev, _this->gainList[_this->gainId]);
                    }
                }
            }
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["directSampling"] = _this->directSamplingMode;
                config.release(true);
            }
        }

        SmGui::LeftLabel("PPM Correction");
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_rtlsdr_ppm_", _this->name), &_this->ppm, 1, 10)) {
            _this->ppm = std::clamp<int>(_this->ppm, -1000000, 1000000);
            if (_this->running) {
                std::lock_guard<std::mutex> devLck(_this->devMtx);
                rtlsdr_set_freq_correction(_this->openDev, _this->ppm);
            }
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["ppm"] = _this->ppm;
                config.release(true);
            }
        }

        // Captured once, the slider must end the disabled state it began
        bool gainLocked = _this->tunerAgc || _this->gainList.size() == 0 || _this->sweeping;
        if (gainLocked) { SmGui::BeginDisabled(); }

        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        SmGui::ForceSync();
        // TODO: FIND ANOTHER WAY
        if (_this->serverMode) {
            if (SmGui::SliderInt(CONCAT("##_rtlsdr_gain_", _this->name), &_this->gainId, 0, _this->gainList.size() - 1, SmGui::FMT_STR_NONE)) {
                _this->updateGainTxt();
                if (_this->running) {
                    std::lock_guard<std::mutex> devLck(_this->devMtx);
                    rtlsdr_set_tuner_gain(_this->openDev, _this->gainList[_this->gainId]);
                }
                if (_this->selectedDevName != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedDevName]["gain"] = _this->gainId;
                    config.release(true);
                }
            }
        }
        else {
            if (ImGui::SliderInt(CONCAT("##_rtlsdr_gain_", _this->name), &_this->gainId, 0, _this->gainList.size() - 1, _this->dbTxt)) {
                _this->updateGainTxt();
                if (_this->running) {
                    std::lock_guard<std::mutex> devLck(_this->devMtx);
                    rtlsdr_set_tuner_gain(_this->openDev, _this->gainList[_this->gainId]);
                }
                if (_this->selectedDevName != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedDevName]["gain"] = _this->gainId;
                    config.release(true);
                }
            }
        }

        if (gainLocked) { SmGui::EndDisabled(); }

        if (!_this->serverMode) { _this->drawGainFinder(); }

        if (SmGui::Checkbox(CONCAT("Bias T##_rtlsdr_rtl_biast_", _this->name), &_this->biasT)) {
            if (_this->running) {
                std::lock_guard<std::mutex> devLck(_this->devMtx);
                rtlsdr_set_bias_tee(_this->openDev, _this->biasT);
            }
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["biasT"] = _this->biasT;
                config.release(true);
            }
        }

        if (SmGui::Checkbox(CONCAT("Offset Tuning##_rtlsdr_rtl_ofs_", _this->name), &_this->offsetTuning)) {
            if (_this->running) {
                std::lock_guard<std::mutex> devLck(_this->devMtx);
                rtlsdr_set_offset_tuning(_this->openDev, _this->offsetTuning);
            }
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["offsetTuning"] = _this->offsetTuning;
                config.release(true);
            }
        }

        if (SmGui::Checkbox(CONCAT("RTL AGC##_rtlsdr_rtl_agc_", _this->name), &_this->rtlAgc)) {
            if (_this->running) {
                std::lock_guard<std::mutex> devLck(_this->devMtx);
                rtlsdr_set_agc_mode(_this->openDev, _this->rtlAgc);
            }
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["rtlAgc"] = _this->rtlAgc;
                config.release(true);
            }
        }

        SmGui::ForceSync();
        if (SmGui::Checkbox(CONCAT("Tuner AGC##_rtlsdr_tuner_agc_", _this->name), &_this->tunerAgc)) {
            if (_this->running) {
                std::lock_guard<std::mutex> devLck(_this->devMtx);
                if (_this->tunerAgc) {
                    rtlsdr_set_tuner_gain_mode(_this->openDev, 0);
                }
                else {
                    rtlsdr_set_tuner_gain_mode(_this->openDev, 1);
                    rtlsdr_set_tuner_gain(_this->openDev, _this->gainList[_this->gainId]);
                }
            }
            if (_this->selectedDevName != "") {
                config.acquire();
                config.conf["devices"][_this->selectedDevName]["tunerAgc"] = _this->tunerAgc;
                config.release(true);
            }
        }
    }

    void worker() {
#ifdef __APPLE__
        // This thread services the USB transfers, if it gets delayed samples are dropped
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        rtlsdr_reset_buffer(openDev);
        if (running) {
            rtlsdr_read_async(openDev, asyncHandler, this, USB_TRANSFER_COUNT, asyncCount);
        }

        // Returning while still running means the stream died, e.g. the dongle was unplugged or lost power
        if (running) { deviceLost = true; }
        workerFinished = true;
    }

    static void frameEventHandler(bool playing, void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        if (_this->sweeping) { _this->sweepStep(playing); }

        if (_this->deviceLost) {
            _this->deviceLost = false;
            if (!_this->selected) { return; }
            flog::warn("RTL-SDR stream died, waiting for the device to come back");
            _this->waitingForDevice = true;
            _this->lastReconnectAttempt = std::chrono::steady_clock::now();
            gui::mainWindow.setPlayState(false);
            return;
        }

        if (!_this->waitingForDevice) { return; }

        // The user started something else in the meantime
        if (!_this->selected || playing) {
            _this->waitingForDevice = false;
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - _this->lastReconnectAttempt < std::chrono::seconds(1)) { return; }
        _this->lastReconnectAttempt = now;

        _this->refresh();
        int id = _this->findDevice(_this->selectedDevName);
        if (id < 0) { return; }

        // It can show up before it can be opened, keep waiting until opening works
        std::string devName = _this->selectedDevName;
        _this->selectById(id);
        if (_this->selectedDevName.empty()) {
            _this->selectedDevName = devName;
            return;
        }

        flog::info("RTL-SDR is back, restarting");
        _this->devId = id;
        core::setInputSampleRate(_this->sampleRate);
        _this->waitingForDevice = false;
        gui::mainWindow.setPlayState(true);
    }

    static void asyncHandler(unsigned char* buf, uint32_t len, void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;

        // There are only 256 possible sample values, so convert them with a table
        static const std::array<float, 256> conversion = []() {
            std::array<float, 256> table;
            for (int i = 0; i < 256; i++) { table[i] = ((float)i - 127.4f) / 128.0f; }
            return table;
        }();

        // Samples at the ends of the 8-bit range, only while the gain finder measures
        if (_this->countClips) {
            uint64_t clipped = 0;
            for (uint32_t i = 0; i < len; i++) { clipped += (buf[i] == 0 || buf[i] == 255); }
            _this->clippedSamples += clipped;
            _this->countedSamples += len;
        }

        int sampCount = len / 2;
        for (int i = 0; i < sampCount; i++) {
            _this->stream.writeBuf[i].re = conversion[buf[i * 2]];
            _this->stream.writeBuf[i].im = conversion[buf[(i * 2) + 1]];
        }
        if (!_this->stream.swap(sampCount)) { return; }
    }

    // "Find Best Gain": steps through every gain, measures each, and sets the best one
    void drawGainFinder() {
        float width = ImGui::GetContentRegionAvail().x;
        bool canRun = running && !tunerAgc && !gainList.empty();
        if (sweeping) {
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "Testing %.1f dB", (float)gainList[std::min<int>(sweepIndex, gainList.size() - 1)] / 10.0f);
            float half = (width - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
            ImGui::ProgressBar((float)sweepIndex / (float)gainList.size(), ImVec2(half, 0), overlay);
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("Cancel##_rtlsdr_gain_cancel_", name), ImVec2(half, 0))) { endSweep("Cancelled, the gain is as it was", true); }
        }
        else {
            if (!canRun) { style::beginDisabled(); }
            if (ImGui::Button(CONCAT("Find Best Gain##_rtlsdr_gain_find_", name), ImVec2(width, 0))) { startSweep(); }
            if (!canRun) { style::endDisabled(); }
            if (!running) { ImGui::TextDisabled("Start the SDR to test the gain"); }
            else if (tunerAgc) { ImGui::TextDisabled("Turn the tuner AGC off to test the gain"); }
        }

        if (!sweepMessage.empty()) { ImGui::TextWrapped("%s", sweepMessage.c_str()); }
        if (sweepResult.empty() || sweeping) { return; }

        // Noise floor against gain, the chosen gain is where its rise levels off
        std::vector<float> floors;
        for (const auto& step : sweepResult) { floors.push_back(step.floorDb); }
        ImGui::PlotLines(CONCAT("##_rtlsdr_gain_plot_", name), floors.data(), (int)floors.size(), 0, "noise floor by gain", FLT_MAX, FLT_MAX, ImVec2(width, 60.0f * style::uiScale));
        if (previousGainId >= 0 && previousGainId != gainId) {
            if (ImGui::Button(CONCAT("Undo##_rtlsdr_gain_undo_", name), ImVec2(width, 0))) {
                applyGain(previousGainId);
                sweepMessage = "Back to " + gainText(gainId);
                previousGainId = -1;
            }
        }
    }

    void startSweep() {
        if (!setTunerGain(gainList[0])) {
            sweepMessage = "The dongle didn't take the gain, try again";
            return;
        }
        sweeping = true;
        sweepIndex = 0;
        sweepSteps.clear();
        sweepResult.clear();
        sweepMessage = "Keep the frequency where you listen while it runs, stop the scanner first";
        previousGainId = gainId;
        sweepCenter = gui::waterfall.getCenterFrequency();
        sweepPhase = SweepPhase::Settling;
        sweepPhaseStart = std::chrono::steady_clock::now();
    }

    void endSweep(const std::string& message, bool restoreGain) {
        countClips = false;
        sweeping = false;
        sweepMessage = message;
        if (restoreGain) {
            setTunerGain(gainList[gainId]);
            previousGainId = -1;
        }
    }

    void sweepStep(bool playing) {
        if (!running || !playing) {
            endSweep("Stopped, the SDR stopped", true);
            return;
        }
        if (std::fabs(gui::waterfall.getCenterFrequency() - sweepCenter) > 1.0) {
            endSweep("Stopped, the frequency changed. Stop the scanner and try again", true);
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (sweepPhase == SweepPhase::Settling) {
            // The new gain takes effect after the samples already in flight
            if (now - sweepPhaseStart < std::chrono::milliseconds(SWEEP_SETTLE_MS)) { return; }
            int size = 0;
            if (gui::waterfall.acquireRawFFT(size, sweepFrame)) { gui::waterfall.releaseRawFFT(); }
            sweepFrames.clear();
            sweepFloors.clear();
            clippedSamples = 0;
            countedSamples = 0;
            countClips = true;
            sweepPhase = SweepPhase::Measuring;
            return;
        }

        int size = 0;
        uint64_t frame = 0;
        float* fft = gui::waterfall.acquireRawFFT(size, frame);
        if (!fft) { return; }
        if (frame == sweepFrame) {
            gui::waterfall.releaseRawFFT();
            return;
        }
        sweepFrame = frame;
        std::vector<float> bins(fft, fft + size);
        gui::waterfall.releaseRawFFT();

        // The dongle's filter rolls off at the edges, the peaks only count in the middle too
        size_t edge = (size_t)(SWEEP_EDGE_SHARE * (float)bins.size());
        sweepFloors.push_back(gain::medianFloor(bins, SWEEP_EDGE_SHARE));
        sweepFrames.push_back(std::vector<float>(bins.begin() + edge, bins.end() - edge));
        if ((int)sweepFrames.size() < SWEEP_FRAMES) { return; }

        countClips = false;
        std::vector<float> floors = sweepFloors;
        std::nth_element(floors.begin(), floors.begin() + floors.size() / 2, floors.end());
        gain::Step step;
        step.gain = gainList[sweepIndex];
        step.floorDb = floors[floors.size() / 2];
        step.clipped = countedSamples ? (double)clippedSamples / (double)countedSamples : 0.0;
        step.steadyPeaks = gain::countSteadyPeaks(sweepFrames, sweepFloors, SWEEP_PEAK_DB, SWEEP_PEAK_SHARE);
        sweepSteps.push_back(step);
        flog::info("RTL-SDR gain test: {0:.1f} dB, floor {1:.1f} dB, clipped {2:.6f}, steady peaks {3}", step.gain / 10.0f, step.floorDb, step.clipped, step.steadyPeaks);

        sweepIndex++;
        if (sweepIndex < (int)gainList.size()) {
            // Measured as this gain only if the dongle took it
            if (!setTunerGain(gainList[sweepIndex])) {
                endSweep("Stopped, the dongle didn't take the gain", true);
                return;
            }
            sweepPhase = SweepPhase::Settling;
            sweepPhaseStart = now;
            return;
        }

        gain::Choice choice = gain::choose(sweepSteps);
        sweepResult = sweepSteps;
        int chosen = (int)(std::find(gainList.begin(), gainList.end(), sweepSteps[choice.step].gain) - gainList.begin());
        applyGain(chosen);
        endSweep("Best gain " + gainText(gainId) + " (was " + gainText(previousGainId) + "). " + choice.reason + ".", false);
    }

    // The tuner's I2C bus shares the USB link with the samples, a write fails now and then while
    // streaming and is retried like a retune
    bool setTunerGain(int tenthsDb) {
        if (!running) { return false; }
        std::lock_guard<std::mutex> devLck(devMtx);
        for (int i = 0; i < TUNE_ATTEMPTS; i++) {
            if (i > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(TUNE_RETRY_MS)); }
            if (rtlsdr_set_tuner_gain(openDev, tenthsDb) == 0) { return true; }
        }
        flog::warn("RTL-SDR failed to set the gain to {0:.1f} dB", tenthsDb / 10.0f);
        return false;
    }

    void applyGain(int id) {
        gainId = std::clamp<int>(id, 0, (int)gainList.size() - 1);
        updateGainTxt();
        setTunerGain(gainList[gainId]);
        if (selectedDevName.empty()) { return; }
        config.acquire();
        config.conf["devices"][selectedDevName]["gain"] = gainId;
        config.release(true);
    }

    std::string gainText(int id) {
        if (id < 0 || id >= (int)gainList.size()) { return "--"; }
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f dB", (float)gainList[id] / 10.0f);
        return buf;
    }

    void updateGainTxt() {
        sprintf(dbTxt, "%.1f dB", (float)gainList[gainId] / 10.0f);
    }

    std::string name;
    rtlsdr_dev_t* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    std::atomic<bool> running = false;

    // More transfers in flight give more slack when the USB thread is delayed, without adding latency
    static constexpr int USB_TRANSFER_COUNT = 48;
    static constexpr double USB_TRANSFER_SECONDS = 0.005;

    EventHandler<bool> frameHandler;
    std::atomic<bool> deviceLost = false;
    std::atomic<bool> workerFinished = true;

    static constexpr int TUNE_ATTEMPTS = 5;
    static constexpr int TUNE_RETRY_MS = 5;
    static constexpr int MAX_FAILED_TUNES = 3;
    int failedTunes = 0;

    // Serialises every librtlsdr control call while streaming
    std::mutex devMtx;
    std::thread tunerThread;
    std::mutex tuneMtx;
    std::condition_variable tuneCnd;
    double pendingFreq = 0.0;
    uint64_t pendingRequest = 0;
    bool tunePending = false;
    bool tunerRunning = false;
    bool waitingForDevice = false;
    bool selected = false;
    std::chrono::steady_clock::time_point lastReconnectAttempt;
    double freq;
    std::string selectedDevName = "";
    int devId = 0;
    int srId = 0;
    int devCount = 0;
    std::thread workerThread;
    bool serverMode = false;

#ifdef __ANDROID__
    int devFd = -1;
#endif

    int ppm = 0;

    bool biasT = false;

    int gainId = 0;
    static constexpr int DEFAULT_GAIN = 338; // tenths of dB

    enum class SweepPhase {
        Settling,
        Measuring
    };
    static constexpr int SWEEP_SETTLE_MS = 250;
    static constexpr int SWEEP_FRAMES = 8;
    static constexpr float SWEEP_EDGE_SHARE = 0.1f;
    static constexpr float SWEEP_PEAK_DB = 12.0f;
    static constexpr float SWEEP_PEAK_SHARE = 0.85f;
    bool sweeping = false;
    int sweepIndex = 0;
    SweepPhase sweepPhase = SweepPhase::Settling;
    std::chrono::steady_clock::time_point sweepPhaseStart;
    double sweepCenter = 0.0;
    uint64_t sweepFrame = 0;
    std::vector<std::vector<float>> sweepFrames;
    std::vector<float> sweepFloors;
    std::vector<gain::Step> sweepSteps;
    std::vector<gain::Step> sweepResult;
    std::string sweepMessage;
    int previousGainId = -1;
    std::atomic<bool> countClips = false;
    std::atomic<uint64_t> clippedSamples = 0;
    std::atomic<uint64_t> countedSamples = 0;
    std::vector<int> gainList;

    bool rtlAgc = false;
    bool tunerAgc = false;
    bool offsetTuning = false;

    int directSamplingMode = 0;

    // Handler stuff
    int asyncCount = 0;

    char dbTxt[128];

    std::vector<std::string> devNames;
    std::string devListTxt;
    std::string sampleRateListTxt;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = 0;
    config.setPath(core::args["root"].s() + "/rtl_sdr_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RTLSDRSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RTLSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
