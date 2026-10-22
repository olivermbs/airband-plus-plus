#include <transcript.h>
#include "check.h"

using namespace scan;

TEST(spelled_numbers_become_digits) {
    CHECK(formatTranscript("police mike proceed inbound to the field qnh one zero one two") == "Police mike proceed inbound to the field QNH 1012");
    CHECK(formatTranscript("the wind is two eight zero degrees three knots") == "The wind is 280 degrees 3 knots");
    CHECK(formatTranscript("contact wien radar one three two decimal seven six five") == "Contact wien radar 132.765");
    CHECK(formatTranscript("nine thousand feet and squawk one five two two") == "9000 feet and squawk 1522");
}

TEST(flight_levels_and_runways) {
    CHECK(formatTranscript("climbing flight level three one zero") == "Climbing FL310");
    CHECK(formatTranscript("backtrack runway one zero left") == "Backtrack runway 10L");
    CHECK(formatTranscript("cleared to land runway two eight") == "Cleared to land runway 28");
}

TEST(callsigns_and_routes_are_joined) {
    CHECK(formatTranscript("OSCAR ECHO NINER FIVE ONE FIVE") == "OE9515");
    CHECK(formatTranscript("lufthansa six kilo alfa climbing") == "Lufthansa 6KA climbing");
    CHECK(formatTranscript("taxi via charlie lima mike") == "Taxi via CLM");
    // A single spelled letter stays a word
    CHECK(formatTranscript("report on delta shortly") == "Report on delta shortly");
}

TEST(numbers_after_fixed_length_words_end_there) {
    CHECK(formatTranscript("squawk one five two two lima five one") == "Squawk 1522 L51");
    CHECK(formatTranscript("qnh one zero one two one zero one two") == "QNH 1012 1012");
    CHECK(formatTranscript("turn left heading two seven zero five miles") == "Turn left heading 270 5 miles");
}

TEST(repeated_callsigns_are_split) {
    CHECK(formatTranscript("cleared to land oscar mike victor oscar mike victor") == "Cleared to land OMV OMV");
    CHECK(formatTranscript("oscar echo niner oscar echo niner five one five") == "OE9 OE9515");
}

TEST(noise_is_dropped) {
    CHECK(formatTranscript("(RADIO STATIC)").empty());
    CHECK(formatTranscript("[static crackling]").empty());
    CHECK(formatTranscript("*pow*").empty());
    CHECK(formatTranscript("BANG").empty());
    CHECK(formatTranscript("I").empty());
    CHECK(formatTranscript("\xe2\x99\xaa").empty());
    CHECK(formatTranscript("*TAKEOFF TAKEOFF TAKEOFF").empty());
    CHECK(formatTranscript("takeoff takeoff takeoff").empty());
    CHECK(formatTranscript("").empty());
}

TEST(resampling_keeps_level_and_length) {
    for (double rate : { 48000.0, 44100.0 }) {
        std::vector<float> in((size_t)rate);
        for (size_t i = 0; i < in.size(); i++) { in[i] = (float)(0.5 * std::sin(2.0 * M_PI * 1000.0 * i / rate)); }
        std::vector<float> out = resampleTo16k(in, rate);
        CHECK(out.size() >= 15990 && out.size() <= 16000);
        double sum = 0.0;
        for (size_t i = 1000; i < out.size() - 1000; i++) { sum += (double)out[i] * out[i]; }
        double rms = std::sqrt(sum / (double)(out.size() - 2000));
        CHECK_NEAR(rms, 0.5 / std::sqrt(2.0), 0.01);
    }
}

TEST(resampling_removes_what_16k_cannot_hold) {
    // 10kHz would fold back to 6kHz
    std::vector<float> in(48000);
    for (size_t i = 0; i < in.size(); i++) { in[i] = (float)(0.5 * std::sin(2.0 * M_PI * 10000.0 * i / 48000.0)); }
    std::vector<float> out = resampleTo16k(in, 48000.0);
    double sum = 0.0;
    for (size_t i = 1000; i < out.size() - 1000; i++) { sum += (double)out[i] * out[i]; }
    CHECK(std::sqrt(sum / (double)(out.size() - 2000)) < 0.01);
}

int main() { return runTests(); }
