#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
namespace core {
constexpr int kPeakBlock = 512;
inline float peakOf(const float* d, int n) { float m = 0; for (int i = 0; i < n; ++i) m = std::max(m, std::abs(d[i])); return m; }
inline int recordBlock(float* dst, int cap, int pos, const float* in, int n) {
    const int c = std::min(n, cap - pos); if (c > 0) { std::memcpy(dst + pos, in, (size_t) c * sizeof(float)); pos += c; } return pos; }
inline void updatePeaks(const float* d, int valid, int& done, std::vector<float>& pk) {
    while ((done + 1) * kPeakBlock <= valid) { pk.push_back(peakOf(d + done * kPeakBlock, kPeakBlock)); ++done; } }
inline void finishPeaks(const float* d, int valid, int done, std::vector<float>& pk) {
    const int s = done * kPeakBlock; if (valid > s) pk.push_back(peakOf(d + s, valid - s)); }
inline float readInterp(const float* d, int len, double pos) {
    if (pos < 0 || pos >= len - 1) return 0.f;
    const int i = (int) pos; const float f = (float) (pos - i);
    return d[i] * (1.f - f) + d[i + 1] * f; }
inline float normGain(float peak, float targetDb) { return peak <= 0.f ? 1.f : std::pow(10.f, targetDb / 20.f) / peak; }

inline double snapBeat(double beat, double grid) { return grid <= 0.0 ? beat : std::round(beat / grid) * grid; }
inline double floorBeat(double beat, double grid) { return grid <= 0.0 ? beat : std::floor(beat / grid) * grid; }
inline void panGains(float pan, float& l, float& r) { l = 1.f - std::max(0.f, pan); r = 1.f + std::min(0.f, pan); }
inline float softLimit(float x) {
    const float a = std::abs(x); if (a <= 0.8f) return x;
    const float y = 0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f); return x < 0.f ? -y : y; }
inline int wrapIndex(long long i, int n) { n = std::max(1, n); const int m = (int) (i % n); return m < 0 ? m + n : m; }
inline bool isDownbeat(long long beatIdx, int beatsPerBar) { return wrapIndex(beatIdx, beatsPerBar) == 0; }
inline void barBeat(double beat, int beatsPerBar, int& bar, int& beatInBar, int& tick) {
    beatsPerBar = std::max(1, beatsPerBar); const double fl = std::floor(beat); const long long bi = (long long) fl;
    bar = (int) std::floor(fl / beatsPerBar) + 1; beatInBar = wrapIndex(bi, beatsPerBar) + 1; tick = (int) ((beat - fl) * 4.0) + 1; }
inline double tapTempo(const std::vector<double>& taps) {
    if (taps.size() < 2) return 0.0;
    size_t first = taps.size() - 1; while (first > 0 && taps[first] - taps[first - 1] <= 2.0 && taps.size() - first < 8) --first;
    if (first == taps.size() - 1) return 0.0;
    const double avg = (taps.back() - taps[first]) / (double) (taps.size() - 1 - first);
    return avg <= 0.0 ? 0.0 : std::min(300.0, std::max(20.0, 60.0 / avg)); }
inline float meterPos(float level) { return std::min(1.f, std::max(0.f, (20.f * std::log10(std::max(level, 1.0e-6f)) + 60.f) / 66.f)); }

inline double midiToFreq(double midi, double a4 = 440.0) { return a4 * std::pow(2.0, (midi - 69.0) / 12.0); }
inline double freqToMidi(double hz, double a4 = 440.0) { return 69.0 + 12.0 * std::log2(hz / a4); }
inline const char* noteName(int midi) { static const char* n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; return n[wrapIndex(midi, 12)]; }
inline int noteOctave(int midi) { return (int) std::floor(midi / 12.0) - 1; }
inline void nearestNote(double hz, double a4, int& midi, double& cents) {
    if (hz <= 0.0) { midi = -1; cents = 0.0; return; }
    const double m = freqToMidi(hz, a4); midi = (int) std::lround(m); cents = (m - midi) * 100.0; }
inline float detectPitch(const float* x, int n, double sr, float fmin = 28.f, float fmax = 1400.f, float thresh = 0.12f) {
    const int tauMin = std::max(2, (int) (sr / fmax)), tauMax = std::min(n / 2, (int) (sr / fmin));
    if (n < 64 || tauMax <= tauMin + 2) return 0.f;
    double e = 0.0; for (int i = 0; i < n; ++i) e += (double) x[i] * x[i];
    if (std::sqrt(e / n) < 0.004) return 0.f;
    const int W = n - tauMax; std::vector<float> d((size_t) tauMax + 2, 0.f);
    for (int t = 1; t <= tauMax; ++t) { double s = 0.0; for (int j = 0; j < W; ++j) { const double q = (double) x[j] - (double) x[j + t]; s += q * q; } d[(size_t) t] = (float) s; }
    d[0] = 1.f; double run = 0.0;
    for (int t = 1; t <= tauMax; ++t) { run += d[(size_t) t]; d[(size_t) t] = run > 0.0 ? (float) ((double) d[(size_t) t] * (double) t / run) : 1.f; }
    int tau = -1;
    for (int t = tauMin; t < tauMax; ++t) if (d[(size_t) t] < thresh) { while (t + 1 < tauMax && d[(size_t) t + 1] < d[(size_t) t]) ++t; tau = t; break; }
    if (tau < 0) return 0.f;
    float tf = (float) tau;
    if (tau > 1 && tau < tauMax) { const float s0 = d[(size_t) tau - 1], s1 = d[(size_t) tau], s2 = d[(size_t) tau + 1], den = 2.f * (s0 + s2 - 2.f * s1); if (std::abs(den) > 1e-9f) tf += (s0 - s2) / den; }
    return (float) (sr / tf); }
}
