#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Finding the RTL-SDR gain with the best signal to noise ratio, independent of the device so it
// can be tested. More gain helps until the receiver's own noise is well above the 8-bit ADC's,
// from there it only raises signals and noise together, and too much overloads the dongle:
// strong stations (FM broadcast) mix into new signals, the noise floor climbs faster than the
// gain, samples clip.
namespace gain {
    struct Step {
        // Tenths of dB, as librtlsdr counts gain
        int gain;
        // Median of the spectrum, transmissions come and go but the median stays on the noise
        float floorDb;
        // Share of 8-bit samples at the ends of the ADC's range
        double clipped;
        // Narrow signals present in nearly every frame: broadcast intermodulation, spurs
        int steadyPeaks;
    };

    struct Choice {
        // Index into the steps, -1 if there is nothing to choose from
        int step = -1;
        // First step that showed overload, -1 if none did
        int overload = -1;
        std::string reason;
    };

    // Noise floor rise over the lowest gain that makes the receiver's noise dominate: the
    // ADC's then adds less than 1dB to it
    constexpr float FLOOR_RISE_DB = 7.0f;
    // A few samples at the rails are harmless, more distort everything
    constexpr double CLIP_LIMIT = 1e-4;
    // Over at least 3dB of gain the floor rises with the gain, much faster is overload
    constexpr float SLOPE_LIMIT = 1.5f;
    constexpr int SLOPE_SPAN = 30;
    // New spurs that stay for two steps in a row
    constexpr int NEW_PEAKS = 3;
    // Gain kept below the overload
    constexpr int OVERLOAD_MARGIN = 30;

    inline int firstOverload(const std::vector<Step>& steps, std::string& reason) {
        int fewestPeaks = steps.empty() ? 0 : steps[0].steadyPeaks;
        for (int i = 0; i < (int)steps.size(); i++) {
            if (steps[i].clipped > CLIP_LIMIT) {
                reason = "samples clip";
                return i;
            }

            for (int j = i - 1; j >= 0; j--) {
                if (steps[i].gain - steps[j].gain < SLOPE_SPAN) { continue; }
                float slope = (steps[i].floorDb - steps[j].floorDb) / ((float)(steps[i].gain - steps[j].gain) / 10.0f);
                if (slope > SLOPE_LIMIT) {
                    reason = "the noise floor climbs faster than the gain";
                    return i;
                }
                break;
            }

            bool spurs = steps[i].steadyPeaks >= fewestPeaks + NEW_PEAKS;
            bool stay = i + 1 < (int)steps.size() && steps[i + 1].steadyPeaks >= fewestPeaks + NEW_PEAKS;
            if (spurs && stay) {
                reason = "new spurs appear";
                return i;
            }
            fewestPeaks = std::min<int>(fewestPeaks, steps[i].steadyPeaks);
        }
        return -1;
    }

    // Steps must be in increasing gain
    inline Choice choose(const std::vector<Step>& steps) {
        Choice choice;
        if (steps.empty()) { return choice; }

        std::string overloadReason;
        choice.overload = firstOverload(steps, overloadReason);

        // Highest gain clear of the overload by the margin
        int clean = (int)steps.size() - 1;
        if (choice.overload >= 0) {
            clean = -1;
            for (int i = 0; i < choice.overload; i++) {
                if (steps[choice.overload].gain - steps[i].gain >= OVERLOAD_MARGIN) { clean = i; }
            }
            if (clean < 0) {
                choice.step = 0;
                choice.reason = "Overloads even at the lowest gains (" + overloadReason + "): the antenna or a strong station nearby needs a filter";
                return choice;
            }
        }

        // Lowest gain where the receiver's noise dominates
        float base = steps[0].floorDb;
        int enough = -1;
        for (int i = 0; i <= clean; i++) {
            if (steps[i].floorDb - base >= FLOOR_RISE_DB) {
                enough = i;
                break;
            }
        }

        if (enough >= 0) {
            choice.step = enough;
            choice.reason = "More gain doesn't improve the signal to noise ratio from here";
        }
        else {
            choice.step = clean;
            choice.reason = (choice.overload >= 0) ? "Highest gain safely below overload" : "Highest gain, the noise floor never rose much (weak antenna or long cable?)";
        }
        if (choice.overload >= 0) { choice.reason += ", overload from " + std::to_string(steps[choice.overload].gain / 10) + "." + std::to_string(steps[choice.overload].gain % 10) + " dB (" + overloadReason + ")"; }
        return choice;
    }

    // Narrow signals in nearly every frame: bins well above the frame's noise floor in at least
    // that share of the frames, neighbouring bins counted as one
    inline int countSteadyPeaks(const std::vector<std::vector<float>>& frames, const std::vector<float>& floors, float aboveDb, float share) {
        if (frames.empty()) { return 0; }
        size_t width = frames[0].size();
        std::vector<int> hits(width, 0);
        for (size_t f = 0; f < frames.size(); f++) {
            for (size_t i = 0; i < width && i < frames[f].size(); i++) {
                if (frames[f][i] > floors[f] + aboveDb) { hits[i]++; }
            }
        }

        int needed = (int)std::ceil(share * (float)frames.size());
        int peaks = 0;
        bool inPeak = false;
        for (size_t i = 0; i < width; i++) {
            bool steady = hits[i] >= needed;
            if (steady && !inPeak) { peaks++; }
            inPeak = steady;
        }
        return peaks;
    }

    // Median of the bins, leaving out the band edges where the dongle's filter rolls off
    inline float medianFloor(std::vector<float> bins, float edgeShare) {
        size_t edge = (size_t)(edgeShare * (float)bins.size());
        if (bins.size() <= 2 * edge) { return NAN; }
        std::vector<float> middle(bins.begin() + edge, bins.end() - edge);
        size_t mid = middle.size() / 2;
        std::nth_element(middle.begin(), middle.begin() + mid, middle.end());
        return middle[mid];
    }
}
