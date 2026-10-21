#include <dsp/noise_reduction/noise_floor_tracker.h>
#include "check.h"
#include <random>

using dsp::noise_reduction::NoiseFloorTracker;

// Blocks of 10ms, like a 15kHz IF delivering 150 samples per block
static const double BLOCK = 0.01;

static void feed(NoiseFloorTracker& t, float level, double seconds, std::mt19937& rng, float jitter = 0.3f) {
    std::normal_distribution<float> noise(0.0f, jitter);
    for (double s = 0; s < seconds; s += BLOCK) {
        t.update(level + noise(rng), BLOCK);
    }
}

TEST(stays_closed_on_noise) {
    std::mt19937 rng(1);
    NoiseFloorTracker t;
    t.setMargin(3.0f);
    feed(t, -40.0f, 10.0, rng);
    CHECK(!t.isOpen());
    CHECK_NEAR(t.getNoiseFloor(), -40.0f, 1.0f);
}

TEST(opens_on_signal_above_margin_and_closes_after) {
    std::mt19937 rng(2);
    NoiseFloorTracker t;
    t.setMargin(3.0f);
    t.setHangTime(0.3);
    feed(t, -40.0f, 5.0, rng);
    CHECK(!t.isOpen());

    t.update(-34.0f, BLOCK);
    CHECK(t.isOpen());

    feed(t, -34.0f, 3.0, rng);
    CHECK(t.isOpen());

    feed(t, -40.0f, 0.1, rng);
    CHECK(t.isOpen()); // hang time keeps it open briefly

    feed(t, -40.0f, 0.5, rng);
    CHECK(!t.isOpen());
}

TEST(ignores_signal_below_margin) {
    std::mt19937 rng(3);
    NoiseFloorTracker t;
    t.setMargin(3.0f);
    feed(t, -40.0f, 5.0, rng);
    feed(t, -38.5f, 0.5, rng, 0.1f);
    CHECK(!t.isOpen());
}

TEST(floor_holds_during_long_transmission) {
    std::mt19937 rng(4);
    NoiseFloorTracker t;
    t.setMargin(3.0f);
    feed(t, -40.0f, 5.0, rng);
    feed(t, -30.0f, 30.0, rng);
    CHECK(t.isOpen());
    CHECK(t.getNoiseFloor() < -37.0f);
}

TEST(floor_follows_noise_down_quickly) {
    std::mt19937 rng(5);
    NoiseFloorTracker t;
    feed(t, -30.0f, 5.0, rng);
    feed(t, -45.0f, 3.0, rng);
    CHECK_NEAR(t.getNoiseFloor(), -45.0f, 1.0f);
}

TEST(floor_follows_noise_up_while_closed) {
    std::mt19937 rng(6);
    NoiseFloorTracker t;
    t.setMargin(3.0f);
    feed(t, -45.0f, 5.0, rng);
    // Small step below the margin, e.g. a gain change: the floor must follow
    feed(t, -43.0f, 5.0, rng);
    CHECK(!t.isOpen());
    CHECK_NEAR(t.getNoiseFloor(), -43.0f, 1.0f);
}

TEST(recovers_from_noise_jump_that_opened_it) {
    std::mt19937 rng(7);
    NoiseFloorTracker t;
    t.setMargin(3.0f);
    feed(t, -45.0f, 5.0, rng);
    feed(t, -40.0f, 120.0, rng);
    CHECK(!t.isOpen());
}

TEST(ignores_silent_blocks) {
    NoiseFloorTracker t;
    t.update(-40.0f, BLOCK);
    t.update(-INFINITY, BLOCK);
    CHECK(t.getNoiseFloor() > -41.0f);
}

int main() { return runTests(); }
