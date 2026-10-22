#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

// Turning what the speech recognition heard into a readable log line, independent of it so it
// can be tested
namespace scan {
    // Whisper wants 16kHz mono. Windowed-sinc low-pass at 7kHz, evaluated at each output instant,
    // for any input rate.
    inline std::vector<float> resampleTo16k(const std::vector<float>& in, double inRate) {
        const double OUT_RATE = 16000.0;
        const double CUTOFF = 7000.0;
        const int HALF_TAPS = 24;

        if (in.empty() || inRate <= 0.0) { return {}; }
        double step = inRate / OUT_RATE;
        size_t count = (size_t)((double)in.size() / step);
        double fc = CUTOFF / inRate;
        std::vector<float> out(count);
        for (size_t n = 0; n < count; n++) {
            double center = (double)n * step;
            long first = (long)std::floor(center) - HALF_TAPS + 1;
            double sum = 0.0;
            double norm = 0.0;
            for (long k = first; k < first + 2 * HALF_TAPS; k++) {
                double x = (double)k - center;
                double sinc = (std::fabs(x) < 1e-9) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * x) / (M_PI * x);
                double window = 0.5 + 0.5 * std::cos(M_PI * x / HALF_TAPS);
                double h = sinc * window;
                norm += h;
                if (k >= 0 && k < (long)in.size()) { sum += h * in[k]; }
            }
            out[n] = (float)(sum / norm);
        }
        return out;
    }

    namespace detail {
        inline const std::map<std::string, char>& digitWords() {
            static const std::map<std::string, char> words = {
                { "zero", '0' }, { "one", '1' }, { "wun", '1' }, { "two", '2' }, { "three", '3' }, { "tree", '3' },
                { "four", '4' }, { "fower", '4' }, { "five", '5' }, { "fife", '5' }, { "six", '6' }, { "seven", '7' },
                { "eight", '8' }, { "nine", '9' }, { "niner", '9' }
            };
            return words;
        }

        inline const std::map<std::string, char>& letterWords() {
            static const std::map<std::string, char> words = {
                { "alfa", 'A' }, { "alpha", 'A' }, { "bravo", 'B' }, { "charlie", 'C' }, { "delta", 'D' }, { "echo", 'E' },
                { "foxtrot", 'F' }, { "golf", 'G' }, { "hotel", 'H' }, { "india", 'I' }, { "juliett", 'J' }, { "juliet", 'J' },
                { "kilo", 'K' }, { "lima", 'L' }, { "mike", 'M' }, { "november", 'N' }, { "oscar", 'O' }, { "papa", 'P' },
                { "quebec", 'Q' }, { "romeo", 'R' }, { "sierra", 'S' }, { "tango", 'T' }, { "uniform", 'U' }, { "victor", 'V' },
                { "whiskey", 'W' }, { "whisky", 'W' }, { "xray", 'X' }, { "x-ray", 'X' }, { "yankee", 'Y' }, { "zulu", 'Z' }
            };
            return words;
        }

        // How many digits follow a word that always takes the same number of them, 0 if any
        inline size_t fixedDigits(const std::string& previous) {
            if (previous == "squawk" || previous == "QNH") { return 4; }
            if (previous == "FL" || previous == "heading") { return 3; }
            if (previous == "runway") { return 2; }
            return 0;
        }

        // A spelled callsign said twice in a row reads as one ("oscar mike victor oscar mike
        // victor"): it starts over where its first letter comes again
        inline std::vector<std::string> splitRepeats(const std::string& run) {
            std::vector<std::string> parts;
            if (run.size() < 4 || !std::isalpha((unsigned char)run[0])) { return { run }; }
            size_t start = 0;
            for (size_t i = 2; i < run.size(); i++) {
                if (run[i] != run[start]) { continue; }
                size_t length = i - start;
                if (run.compare(i, std::min(length, run.size() - i), run, start, std::min(length, run.size() - i)) != 0) { continue; }
                parts.push_back(run.substr(start, length));
                start = i;
            }
            parts.push_back(run.substr(start));
            return parts;
        }

        inline bool isAbbreviation(const std::string& word) {
            static const char* LIST[] = { "qnh", "qfe", "ils", "vfr", "ifr", "atis", "vor", "dme", "ndb", "rnav", "tcas", "gps", "atc", "eta", "metar", "cavok", "vmc", "imc" };
            for (const char* a : LIST) {
                if (word == a) { return true; }
            }
            return false;
        }

        // Words in the recognizer's own markup: (static), [music], *pow*, a note symbol
        inline std::string stripMarkup(const std::string& text) {
            std::string out;
            char closing = 0;
            for (size_t i = 0; i < text.size(); i++) {
                char c = text[i];
                if (closing) {
                    if (c == closing) { closing = 0; }
                    continue;
                }
                if (c == '(') { closing = ')'; continue; }
                if (c == '[') { closing = ']'; continue; }
                if (c == '*') { closing = '*'; continue; }
                // Multi-byte characters (music notes and the like) are no words
                if ((unsigned char)c >= 0x80) { out += ' '; continue; }
                out += c;
            }
            return out;
        }
    }

    // A readable line from what the recognizer heard, or empty when it heard nothing worth
    // keeping. Spelled numbers become digits, spelled letters and numbers together a callsign or
    // a route, "flight level" FL, abbreviations upper case. On noise the recognizer invents
    // a word or two, markup or one word repeated, that is dropped.
    inline std::string formatTranscript(const std::string& raw) {
        const size_t MIN_WORDS = 2;

        std::string text = detail::stripMarkup(raw);
        std::vector<std::string> words;
        std::string word;
        for (char c : text) {
            bool keep = std::isalnum((unsigned char)c) || c == '-' || c == '\'';
            if (keep) {
                word += (char)std::tolower((unsigned char)c);
                continue;
            }
            if (!word.empty()) { words.push_back(word); }
            word.clear();
        }
        if (!word.empty()) { words.push_back(word); }

        size_t distinct = 0;
        for (size_t i = 0; i < words.size(); i++) {
            if (std::find(words.begin(), words.begin() + i, words[i]) == words.begin() + i) { distinct++; }
        }
        if (words.size() < MIN_WORDS || distinct < MIN_WORDS) { return ""; }

        const auto& digits = detail::digitWords();
        const auto& letters = detail::letterWords();
        std::vector<std::string> out;
        for (size_t i = 0; i < words.size();) {
            // A run of spelled digits and letters, "decimal" between digits
            std::string run;
            size_t letterCount = 0;
            size_t digitCount = 0;
            size_t maxDigits = out.empty() ? 0 : detail::fixedDigits(out.back());
            size_t j = i;
            while (j < words.size()) {
                if (maxDigits && (digitCount == maxDigits || letters.count(words[j]))) { break; }
                auto d = digits.find(words[j]);
                auto l = letters.find(words[j]);
                if (d != digits.end()) {
                    run += d->second;
                    digitCount++;
                }
                else if (l != letters.end()) {
                    run += l->second;
                    letterCount++;
                }
                else if ((words[j] == "decimal" || words[j] == "point") && !run.empty() && std::isdigit((unsigned char)run.back()) && j + 1 < words.size() && digits.count(words[j + 1])) {
                    run += '.';
                }
                else if (words[j] == "thousand" && !run.empty() && std::isdigit((unsigned char)run.back())) {
                    run += "000";
                }
                else if (words[j] == "hundred" && !run.empty() && std::isdigit((unsigned char)run.back())) {
                    run += "00";
                }
                else if (std::all_of(words[j].begin(), words[j].end(), [](char c) { return std::isdigit((unsigned char)c); })) {
                    // Numbers the recognizer already wrote as digits
                    run += words[j];
                }
                else { break; }
                j++;
            }

            // A single spelled letter is a word ("taxi via delta"), a letter or digit run isn't
            bool single = (j - i == 1) && letterCount == 1;
            if (j == i || single) {
                std::string w = words[i];
                if (detail::isAbbreviation(w)) { std::transform(w.begin(), w.end(), w.begin(), ::toupper); }
                if (w == "flight" && i + 1 < words.size() && words[i + 1] == "level") {
                    out.push_back("FL");
                    i += 2;
                    continue;
                }
                out.push_back(w);
                i++;
                continue;
            }

            // Runway designators: "runway two eight left" is 28L
            if (j < words.size() && !out.empty() && out.back() == "runway" && letterCount == 0) {
                const std::string& side = words[j];
                if (side == "left") { run += 'L'; j++; }
                else if (side == "right") { run += 'R'; j++; }
                else if (side == "center" || side == "centre") { run += 'C'; j++; }
            }

            // "FL" and its number belong together
            if (!out.empty() && out.back() == "FL" && letterCount == 0) { out.back() += run; }
            else {
                for (const auto& part : detail::splitRepeats(run)) { out.push_back(part); }
            }
            i = j;
        }

        std::string line;
        for (const auto& w : out) {
            if (!line.empty()) { line += ' '; }
            line += w;
        }
        if (!line.empty()) { line[0] = (char)std::toupper((unsigned char)line[0]); }
        return line;
    }
}
