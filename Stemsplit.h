#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>
namespace stem {

struct Settings {
    int   fftSize     = 2048;
    float sharpness   = 2.f;
    float harmSmoothMs = 190.f;
    float percWidthHz  = 700.f;
    float bassHz       = 180.f;
    bool  drums = true, bass = true, melody = true;
    bool  zones = false;
    float guitarTopHz = 500.f, pianoTopHz = 1500.f, vocalTopHz = 4000.f;
    bool  guitar = true, piano = true, vocals = true, other = true;
};

struct Result { std::vector<float> drums, bass, melody, guitar, piano, vocals, other; };

inline void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * 3.14159265358979323846 / (double) len * (inverse ? 1 : -1);
        const std::complex<float> wl((float) std::cos(ang), (float) std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) { const auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } } }
    if (inverse) for (auto& x : a) x /= (float) n;
}

inline float medianAt(const float* base, int count, int stride, int c, int half, std::vector<float>& tmp) {
    tmp.resize((size_t) (2 * half + 1));
    for (int k = -half; k <= half; ++k) tmp[(size_t) (k + half)] = base[(size_t) std::min(count - 1, std::max(0, c + k)) * (size_t) stride];
    std::nth_element(tmp.begin(), tmp.begin() + half, tmp.end()); return tmp[(size_t) half];
}

inline Result split(const float* x, int n, double sr, const Settings& s,
                    const std::function<void(float)>& progress = {}, const std::atomic<bool>* cancel = nullptr) {
    Result out; if (n <= 0) return out;
    const int N = std::max(512, s.fftSize), H = N / 4, B = N / 2 + 1;
    const int pad = N, total = n + 2 * pad, frames = (total - N) / H + 2;
    std::vector<float> win((size_t) N); for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * (float) i / (float) N);
    const float olaNorm = 1.f / 1.5f;
    std::vector<float> sig((size_t) frames * (size_t) H + (size_t) N, 0.f); std::copy(x, x + n, sig.begin() + pad);
    std::vector<std::complex<float>> spec((size_t) frames * (size_t) B), buf((size_t) N);
    std::vector<float> mag((size_t) frames * (size_t) B);
    auto stop = [&] { return cancel != nullptr && cancel->load(); };
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < N; ++i) buf[(size_t) i] = { sig[(size_t) f * (size_t) H + (size_t) i] * win[(size_t) i], 0.f };
        fft(buf, false);
        for (int b = 0; b < B; ++b) { spec[(size_t) f * (size_t) B + (size_t) b] = buf[(size_t) b]; mag[(size_t) f * (size_t) B + (size_t) b] = std::abs(buf[(size_t) b]); }
        if ((f & 63) == 0) { if (progress) progress(0.15f * (float) f / (float) frames); if (stop()) return {}; } }

    const double frameSec = (double) H / sr, binHz = sr / (double) N;
    const int hT = std::max(1, (int) std::lround(s.harmSmoothMs / 1000.0 / frameSec / 2.0)), hF = std::max(1, (int) std::lround(s.percWidthHz / binHz / 2.0));
    std::vector<float> Hm((size_t) frames * (size_t) B), Pm((size_t) frames * (size_t) B), tmp;
    for (int f = 0; f < frames; ++f) {
        for (int b = 0; b < B; ++b) { Hm[(size_t) f * (size_t) B + (size_t) b] = medianAt(mag.data() + b, frames, B, f, hT, tmp); Pm[(size_t) f * (size_t) B + (size_t) b] = medianAt(mag.data() + (size_t) f * (size_t) B, B, 1, b, hF, tmp); }
        if ((f & 31) == 0) { if (progress) progress(0.15f + 0.55f * (float) f / (float) frames); if (stop()) return {}; } }

    auto lowCurve = [&](double fc) { std::vector<float> w((size_t) B); for (int b = 0; b < B; ++b) { const double hz = std::max(1.0, b * binHz); w[(size_t) b] = (float) (1.0 / (1.0 + std::pow(hz / fc, 6.0))); } return w; };
    const double fc0 = std::max(30.0, (double) s.bassHz);
    const std::vector<float> lowW = lowCurve(fc0);
    std::vector<float> c1, c2, c3;
    if (s.zones) { const double fc1 = std::max(fc0 * 1.25, (double) s.guitarTopHz), fc2 = std::max(fc1 * 1.25, (double) s.pianoTopHz), fc3 = std::max(fc2 * 1.25, (double) s.vocalTopHz); c1 = lowCurve(fc1); c2 = lowCurve(fc2); c3 = lowCurve(fc3); }
    const float pw = std::max(1.f, s.sharpness);
    std::vector<float> mpv(spec.size());
    for (size_t i = 0; i < spec.size(); ++i) { const float h = std::pow(Hm[i], pw), p = std::pow(Pm[i], pw), den = h + p; mpv[i] = den > 1e-20f ? p / den : 0.5f; }
    { std::vector<float>().swap(Hm); std::vector<float>().swap(Pm); }

    auto synth = [&](auto&& mult, std::vector<float>& dst, float p0, float p1) {
        std::vector<float> acc((size_t) frames * (size_t) H + (size_t) N, 0.f);
        for (int f = 0; f < frames; ++f) {
            for (int b = 0; b < B; ++b) { const size_t i = (size_t) f * (size_t) B + (size_t) b; buf[(size_t) b] = spec[i] * mult(i, b); }
            for (int b = 1; b < N / 2; ++b) buf[(size_t) (N - b)] = std::conj(buf[(size_t) b]);
            buf[(size_t) (N / 2)].imag(0.f); buf[0].imag(0.f);
            fft(buf, true);
            for (int i = 0; i < N; ++i) acc[(size_t) f * (size_t) H + (size_t) i] += buf[(size_t) i].real() * win[(size_t) i];
            if ((f & 63) == 0 && progress) progress(p0 + (p1 - p0) * (float) f / (float) frames); }
        dst.assign(acc.begin() + pad, acc.begin() + pad + n); for (auto& v : dst) v *= olaNorm; };
    const int nOut = s.zones ? (int) s.drums + (int) s.bass + (int) s.guitar + (int) s.piano + (int) s.vocals + (int) s.other : (int) s.drums + (int) s.bass + (int) s.melody;
    int k = 0;
    auto emit = [&](bool on, std::vector<float>& dst, auto&& mult) -> bool {
        if (! on) return true;
        const float p0 = 0.70f + 0.30f * (float) k / (float) nOut, p1 = 0.70f + 0.30f * (float) (k + 1) / (float) nOut; ++k;
        synth(mult, dst, p0, p1); return ! stop(); };
    if (! emit(s.drums, out.drums, [&](size_t i, int) { return mpv[i]; })) return {};
    if (! emit(s.bass, out.bass, [&](size_t i, int b) { return (1.f - mpv[i]) * lowW[(size_t) b]; })) return {};
    if (! s.zones) { if (! emit(s.melody, out.melody, [&](size_t i, int b) { return (1.f - mpv[i]) * (1.f - lowW[(size_t) b]); })) return {}; }
    else {
        if (! emit(s.guitar, out.guitar, [&](size_t i, int b) { return (1.f - mpv[i]) * (c1[(size_t) b] - lowW[(size_t) b]); })) return {};
        if (! emit(s.piano,  out.piano,  [&](size_t i, int b) { return (1.f - mpv[i]) * (c2[(size_t) b] - c1[(size_t) b]); })) return {};
        if (! emit(s.vocals, out.vocals, [&](size_t i, int b) { return (1.f - mpv[i]) * (c3[(size_t) b] - c2[(size_t) b]); })) return {};
        if (! emit(s.other,  out.other,  [&](size_t i, int b) { return (1.f - mpv[i]) * (1.f - c3[(size_t) b]); })) return {};
    }
    if (progress) progress(1.f);
    return out;
}
}
