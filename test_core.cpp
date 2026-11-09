// Unit tests for AudioCore.h and Stemsplit.h (no JUCE required).
// Build:  cmake -DMINIDAW_BUILD_TESTS=ON .. && cmake --build . --target test_core && ctest
//    or:  g++ -std=c++17 -O1 -I. test_core.cpp -o test_core && ./test_core
#include "AudioCore.h"
#include "Stemsplit.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int g_fail = 0, g_total = 0;
#define CHECK(c) do { ++g_total; if (!(c)) { ++g_fail; std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(std::abs((double) (a) - (double) (b)) <= (tol))

static const double kPi = 3.14159265358979323846;
static std::vector<float> sine(double hz, double sr, int n, float amp = 0.5f) {
    std::vector<float> v((size_t) n); for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin(2.0 * kPi * hz * i / sr); return v; }

static void testPeaksAndRecording() {
    float d[4] = { 0.1f, -0.7f, 0.3f, 0.f }; NEAR(core::peakOf(d, 4), 0.7, 1e-6);
    float dst[8] = {}; const float in[5] = { 1, 2, 3, 4, 5 };
    int pos = core::recordBlock(dst, 8, 0, in, 5); CHECK(pos == 5);
    pos = core::recordBlock(dst, 8, pos, in, 5); CHECK(pos == 8);              // clipped at capacity, no overflow
    CHECK(dst[7] == 3.f);
    pos = core::recordBlock(dst, 8, pos, in, 5); CHECK(pos == 8);              // full buffer stays full
    std::vector<float> sig(core::kPeakBlock * 3 + 100, 0.25f), pk; int done = 0;
    core::updatePeaks(sig.data(), (int) sig.size(), done, pk); CHECK(done == 3 && pk.size() == 3);
    core::finishPeaks(sig.data(), (int) sig.size(), done, pk); CHECK(pk.size() == 4);
    NEAR(pk.back(), 0.25, 1e-6);
}
static void testInterpAndGain() {
    const float d[4] = { 0.f, 1.f, 0.f, -1.f };
    NEAR(core::readInterp(d, 4, 0.5), 0.5, 1e-6); NEAR(core::readInterp(d, 4, 1.0), 1.0, 1e-6);
    CHECK(core::readInterp(d, 4, -0.1) == 0.f); CHECK(core::readInterp(d, 4, 3.0) == 0.f);   // out of range = silence
    NEAR(core::normGain(0.5f, 0.f), 2.0, 1e-5); CHECK(core::normGain(0.f, -1.f) == 1.f);
    NEAR(core::normGain(1.f, -6.f), 0.5012, 1e-3);
}
static void testGridAndPan() {
    NEAR(core::snapBeat(1.3, 0.5), 1.5, 1e-9); NEAR(core::snapBeat(1.3, 0.0), 1.3, 1e-9); NEAR(core::floorBeat(1.9, 0.5), 1.5, 1e-9);
    float l, r; core::panGains(0.f, l, r); CHECK(l == 1.f && r == 1.f);
    core::panGains(1.f, l, r); CHECK(l == 0.f && r == 1.f); core::panGains(-1.f, l, r); CHECK(l == 1.f && r == 0.f);
}
static void testLimiter() {
    NEAR(core::softLimit(0.5f), 0.5, 1e-9); NEAR(core::softLimit(-0.5f), -0.5, 1e-9);
    CHECK(core::softLimit(5.f) <= 1.0001f && core::softLimit(5.f) > 0.9f); CHECK(core::softLimit(-5.f) >= -1.0001f);
    float prev = -2.f; for (float x = 0.f; x < 4.f; x += 0.05f) { const float y = core::softLimit(x); CHECK(y >= prev - 1e-6f); prev = y; }   // monotonic
}
static void testBarMath() {
    CHECK(core::wrapIndex(-1, 4) == 3); CHECK(core::wrapIndex(5, 4) == 1); CHECK(core::wrapIndex(3, 0) == 0);
    CHECK(core::isDownbeat(0, 4) && core::isDownbeat(8, 4) && ! core::isDownbeat(3, 4));
    int bar, beat, tick; core::barBeat(0.0, 4, bar, beat, tick); CHECK(bar == 1 && beat == 1 && tick == 1);
    core::barBeat(5.5, 4, bar, beat, tick); CHECK(bar == 2 && beat == 2 && tick == 3);
}
static void testTapTempo() {
    CHECK(core::tapTempo({}) == 0.0); CHECK(core::tapTempo({ 1.0 }) == 0.0);
    NEAR(core::tapTempo({ 0.0, 0.5, 1.0, 1.5 }), 120.0, 0.01);
    NEAR(core::tapTempo({ 0.0, 0.5, 10.0, 10.5, 11.0 }), 120.0, 0.01);          // an old gap resets the average
}
static void testMeter() {
    NEAR(core::meterPos(1.f), 60.0 / 66.0, 1e-4); CHECK(core::meterPos(0.f) == 0.f); CHECK(core::meterPos(100.f) == 1.f);
}
static void testPitch() {
    NEAR(core::midiToFreq(69), 440.0, 1e-9); NEAR(core::freqToMidi(880.0), 81.0, 1e-9);
    CHECK(std::string(core::noteName(60)) == "C" && std::string(core::noteName(61)) == "C#"); CHECK(core::noteOctave(60) == 4 && core::noteOctave(59) == 3);
    int midi; double cents; core::nearestNote(442.0, 440.0, midi, cents); CHECK(midi == 69); NEAR(cents, 7.85, 0.1);
    core::nearestNote(0.0, 440.0, midi, cents); CHECK(midi == -1);
    const double sr = 44100.0;
    for (double hz : { 82.41, 110.0, 220.0, 440.0, 659.25 }) { auto s = sine(hz, sr, 4096); const float f = core::detectPitch(s.data(), (int) s.size(), sr); NEAR(f, hz, hz * 0.01); }
    std::vector<float> silence(4096, 0.f); CHECK(core::detectPitch(silence.data(), 4096, sr) == 0.f);
}

// ---- stem splitter (DSP engine)
static void testStemSplit() {
    const double sr = 44100.0; const int n = (int) sr * 2;
    auto x = sine(60.0, sr, n, 0.4f); auto mid = sine(1000.0, sr, n, 0.3f); for (int i = 0; i < n; ++i) x[(size_t) i] += mid[(size_t) i];
    for (int k = 0; k < 4; ++k) { const int at = (int) (sr * (0.2 + 0.5 * k)); for (int i = 0; i < 400; ++i) x[(size_t) (at + i)] += 0.8f * std::exp(-i / 60.f) * ((i * 7919) % 200 / 100.f - 1.f); }   // noisy hits
    stem::Settings s; s.zones = false; auto r = stem::split(x.data(), n, sr, s);
    CHECK((int) r.drums.size() == n && (int) r.bass.size() == n && (int) r.melody.size() == n);
    double err = 0, ref = 0; for (int i = 0; i < n; ++i) { const double y = r.drums[(size_t) i] + r.bass[(size_t) i] + r.melody[(size_t) i]; err += (y - x[(size_t) i]) * (y - x[(size_t) i]); ref += (double) x[(size_t) i] * x[(size_t) i]; }
    CHECK(err / ref < 0.01);                                                   // stems add back up to the input
    auto rms = [](const std::vector<float>& v, int a, int b) { double e = 0; for (int i = a; i < b; ++i) e += (double) v[(size_t) i] * v[(size_t) i]; return std::sqrt(e / (b - a)); };
    CHECK(rms(r.bass, n / 4, n * 3 / 4) > rms(r.melody, n / 4, n * 3 / 4) * 0.8);     // the 60 Hz tone lands in bass
    std::atomic<bool> cancel { true }; auto c = stem::split(x.data(), n, sr, s, {}, &cancel); CHECK(c.drums.empty() && c.bass.empty());   // cancel works
    CHECK(stem::split(x.data(), 0, sr, s).drums.empty());                      // empty input is safe
}

int main() {
    testPeaksAndRecording(); testInterpAndGain(); testGridAndPan(); testLimiter(); testBarMath(); testTapTempo(); testMeter(); testPitch(); testStemSplit();
    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
