#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  AmpSim.h — built-in guitar amp / cabinet / pedalboard simulation
//  Pure DSP, no JUCE, no Qt, no PortAudio.
//  v2: 4x-oversampled cascaded tube stages, tone stack before the last gain stage,
//      sagging power amp, speaker/mic cabinet model, built-in reverb, oversampled pedals.
// ═══════════════════════════════════════════════════════════════════════════
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ampsim {

constexpr double kPi = 3.14159265358979323846;

// ─── Biquad ────────────────────────────────────────────────────────────────
struct BqCoeffs { double b[3] = {1,0,0}; double a[3] = {1,0,0}; };

inline BqCoeffs bq_peaking(double fs, double f0, double g, double Q) {
    const double A  = std::pow(10.0, g / 40.0);
    const double w  = 2 * kPi * f0 / fs;
    const double cs = std::cos(w), al = std::sin(w) / (2 * Q);
    BqCoeffs c;
    const double a0 = 1 + al / A;
    c.b[0] = (1 + al * A) / a0; c.b[1] = -2 * cs / a0; c.b[2] = (1 - al * A) / a0;
    c.a[0] = 1.0;               c.a[1] = -2 * cs / a0; c.a[2] = (1 - al / A) / a0;
    return c;
}
inline BqCoeffs bq_low_shelf(double fs, double f0, double g, double S = 0.9) {
    const double A  = std::pow(10.0, g / 40.0);
    const double w  = 2 * kPi * f0 / fs;
    const double cs = std::cos(w), sn = std::sin(w);
    const double al = sn / 2.0 * std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
    const double tw = 2.0 * std::sqrt(A) * al;
    BqCoeffs c;
    const double a0 = (A + 1) + (A - 1) * cs + tw;
    c.b[0] = A * ((A + 1) - (A - 1) * cs + tw) / a0;
    c.b[1] = 2 * A * ((A - 1) - (A + 1) * cs) / a0;
    c.b[2] = A * ((A + 1) - (A - 1) * cs - tw) / a0;
    c.a[0] = 1.0;
    c.a[1] = -2 * ((A - 1) + (A + 1) * cs) / a0;
    c.a[2] = ((A + 1) + (A - 1) * cs - tw) / a0;
    return c;
}
inline BqCoeffs bq_high_shelf(double fs, double f0, double g, double S = 0.9) {
    const double A  = std::pow(10.0, g / 40.0);
    const double w  = 2 * kPi * f0 / fs;
    const double cs = std::cos(w), sn = std::sin(w);
    const double al = sn / 2.0 * std::sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
    const double tw = 2.0 * std::sqrt(A) * al;
    BqCoeffs c;
    const double a0 = (A + 1) - (A - 1) * cs + tw;
    c.b[0] = A * ((A + 1) + (A - 1) * cs + tw) / a0;
    c.b[1] = -2 * A * ((A - 1) + (A + 1) * cs) / a0;
    c.b[2] = A * ((A + 1) + (A - 1) * cs - tw) / a0;
    c.a[0] = 1.0;
    c.a[1] = 2 * ((A - 1) - (A + 1) * cs) / a0;
    c.a[2] = ((A + 1) - (A - 1) * cs - tw) / a0;
    return c;
}
inline BqCoeffs bq_lowpass(double fs, double f0, double Q = 0.707) {
    const double w = 2 * kPi * f0 / fs, cs = std::cos(w), al = std::sin(w) / (2 * Q);
    BqCoeffs c;
    const double a0 = 1 + al;
    c.b[0] = (1 - cs) / 2.0 / a0; c.b[1] = (1 - cs) / a0; c.b[2] = (1 - cs) / 2.0 / a0;
    c.a[0] = 1.0;                 c.a[1] = -2 * cs / a0;   c.a[2] = (1 - al) / a0;
    return c;
}
inline BqCoeffs bq_highpass(double fs, double f0, double Q = 0.707) {
    const double w = 2 * kPi * f0 / fs, cs = std::cos(w), al = std::sin(w) / (2 * Q);
    BqCoeffs c;
    const double a0 = 1 + al;
    c.b[0] = (1 + cs) / 2.0 / a0; c.b[1] = -(1 + cs) / a0; c.b[2] = (1 + cs) / 2.0 / a0;
    c.a[0] = 1.0;                 c.a[1] = -2 * cs / a0;    c.a[2] = (1 - al) / a0;
    return c;
}

class Biquad {
public:
    double b[3] = {1,0,0}, a[3] = {1,0,0}, z1 = 0, z2 = 0;
    void set(const BqCoeffs& c) { for (int i = 0; i < 3; ++i) { b[i] = c.b[i]; a[i] = c.a[i]; } }
    void reset() { z1 = z2 = 0; }
    inline float tick(float x) {
        const double y = b[0] * x + z1;
        z1 = b[1] * x - a[1] * y + z2;
        z2 = b[2] * x - a[2] * y;
        return (float) y;
    }
};

// ─── Modulated delay (chorus / flanger) ────────────────────────────────────
class ModDelay {
public:
    explicit ModDelay(int size = 4096) : size_(size), line_(size, 0.f) {}
    void clear() { std::fill(line_.begin(), line_.end(), 0.f); }
    void process(const float* x, const float* delay, float fb, float* out, int n) {
        constexpr int CH = 16;
        const int H = size_;
        for (int s = 0; s < n; s += CH) {
            const int m = std::min(CH, n - s);
            for (int i = 0; i < m; ++i) {
                double d = std::max(1.0, std::min((double)(H - 2), (double)delay[s + i]));
                double pos = (double)(H - 1) - d;
                int i0 = (int) std::floor(pos);
                double fr = pos - i0;
                if (i0 < 0) i0 = 0;
                if (i0 > H - 2) i0 = H - 2;
                out[s + i] = line_[(size_t)i0] * (1.f - (float)fr) + line_[(size_t)(i0 + 1)] * (float)fr;
            }
            std::memmove(&line_[0], &line_[(size_t)m], (size_t)(H - m) * sizeof(float));
            for (int i = 0; i < m; ++i) line_[(size_t)(H - m + i)] = x[s + i] + fb * out[s + i];
        }
    }
private:
    int size_;
    std::vector<float> line_;
};

// ─── Oversampling (linear-phase half-band FIR stages) ──────────────────────
inline double besselI0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 60; ++k) { const double q = x / (2.0 * k); t *= q * q; s += t; if (t < 1e-15 * s) break; }
    return s;
}

class HalfBand {
public:
    explicit HalfBand(int M = 15, double beta = 7.0) {
        const int K = 2 * M + 1;
        std::vector<double> h((size_t) K, 0.0);
        const double i0b = besselI0(beta);
        double sum = 0.0;
        for (int t = 0; t < K; ++t) {
            const int k = t - M;
            double v = 0.0;
            if (k == 0) v = 0.5;
            else if ((k & 1) != 0) v = std::sin(kPi * k * 0.5) / (kPi * k);
            const double r = (double) k / (double) M;
            h[(size_t) t] = v * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            sum += h[(size_t) t];
        }
        for (int t = 0; t < K; ++t) {
            h[(size_t) t] /= sum;
            if (h[(size_t) t] != 0.0) { ti_.push_back(t); tv_.push_back(h[(size_t) t]); }
        }
        hist_.assign((size_t) (2 * M), 0.f);
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.f); }

    // n input samples -> 2n output samples
    void up(const float* in, float* out, int n) {
        if (n <= 0) return;
        const int H = (int) hist_.size();
        work_.resize((size_t) (H + 2 * n));
        std::copy(hist_.begin(), hist_.end(), work_.begin());
        for (int i = 0; i < n; ++i) { work_[(size_t) (H + 2 * i)] = 2.f * in[i]; work_[(size_t) (H + 2 * i + 1)] = 0.f; }
        const size_t nt = ti_.size();
        for (int j = 0; j < 2 * n; ++j) {
            const float* w = work_.data() + H + j;
            double acc = 0.0;
            for (size_t q = 0; q < nt; ++q) acc += tv_[q] * (double) w[-ti_[q]];
            out[j] = (float) acc;
        }
        std::copy(work_.begin() + 2 * n, work_.begin() + 2 * n + H, hist_.begin());
    }
    // 2n input samples -> n output samples
    void down(const float* in, float* out, int n) {
        if (n <= 0) return;
        const int H = (int) hist_.size();
        work_.resize((size_t) (H + 2 * n));
        std::copy(hist_.begin(), hist_.end(), work_.begin());
        std::copy(in, in + 2 * n, work_.begin() + H);
        const size_t nt = ti_.size();
        for (int i = 0; i < n; ++i) {
            const float* w = work_.data() + H + 2 * i;
            double acc = 0.0;
            for (size_t q = 0; q < nt; ++q) acc += tv_[q] * (double) w[-ti_[q]];
            out[i] = (float) acc;
        }
        std::copy(work_.begin() + 2 * n, work_.begin() + 2 * n + H, hist_.begin());
    }
private:
    std::vector<int> ti_;
    std::vector<double> tv_;
    std::vector<float> hist_, work_;
};

// 2x oversampler used around the nonlinear part of the stomp pedals
class Oversampler2 {
public:
    void reset() { up_.reset(); dn_.reset(); }
    template <class F>
    void run(const float* in, float* out, int n, F&& f) {
        if (n <= 0) return;
        buf_.resize((size_t) (2 * n));
        up_.up(in, buf_.data(), n);
        for (int i = 0; i < 2 * n; ++i) buf_[(size_t) i] = f(buf_[(size_t) i]);
        dn_.down(buf_.data(), out, n);
    }
private:
    HalfBand up_ { 15, 7.0 }, dn_ { 15, 7.0 };
    std::vector<float> buf_;
};

// 4x oversampler for the amp's gain stages (two cascaded half-band stages)
class Oversampler4 {
public:
    void reset() { upA_.reset(); upB_.reset(); dnB_.reset(); dnA_.reset(); }
    void up(const float* in, float* out4, int n) {
        if (n <= 0) return;
        t_.resize((size_t) (2 * n));
        upA_.up(in, t_.data(), n);
        upB_.up(t_.data(), out4, 2 * n);
    }
    void down(const float* in4, float* out, int n) {
        if (n <= 0) return;
        t_.resize((size_t) (2 * n));
        dnB_.down(in4, t_.data(), 2 * n);
        dnA_.down(t_.data(), out, n);
    }
private:
    HalfBand upA_ { 15, 7.0 }, upB_ { 7, 6.0 }, dnB_ { 7, 6.0 }, dnA_ { 15, 7.0 };
    std::vector<float> t_;
};

// ─── Amp parameters ────────────────────────────────────────────────────────
struct AmpParams {
    float input_gain = 1.0f;
    float drive      = 0.35f;
    float asym       = 0.0f;
    float bass_db    = 0.0f;
    float mid_db     = 0.0f;
    float treble_db  = 0.0f;
    float cab_res_db = 0.0f;
    float cab_lp_hz  = 5500.0f;
    float presence_db = 0.0f;
    float master     = 0.6f;
    bool  noise_gate_enabled = false;
    float noise_gate_thresh_db = -50.0f;
    // added in the 4x-oversampled engine
    int   voice  = 1;       // 0 American clean · 1 British crunch · 2 Chime · 3 Modern high-gain
    float tight  = 0.30f;   // low-cut ahead of the gain stages (0..1)
    float sag    = 0.35f;   // power-amp push + supply sag (0..1)
    float mic    = 0.30f;   // cab mic position, 0 = on-axis/bright … 1 = off-axis/dark
    float reverb = 0.0f;    // built-in room/spring reverb mix (0..1)

    bool operator==(const AmpParams& o) const {
        return input_gain == o.input_gain && drive == o.drive && asym == o.asym && bass_db == o.bass_db
            && mid_db == o.mid_db && treble_db == o.treble_db && cab_res_db == o.cab_res_db
            && cab_lp_hz == o.cab_lp_hz && presence_db == o.presence_db && master == o.master
            && noise_gate_enabled == o.noise_gate_enabled && noise_gate_thresh_db == o.noise_gate_thresh_db
            && voice == o.voice && tight == o.tight && sag == o.sag && mic == o.mic && reverb == o.reverb;
    }
    bool operator!=(const AmpParams& o) const { return !(*this == o); }
};

namespace detail {
struct OnePoleLP {
    double a = 0.5, z = 0.0;
    void set(double fc, double fs) { a = 1.0 - std::exp(-2.0 * kPi * std::min(fc, fs * 0.45) / fs); }
    void reset() { z = 0.0; }
    inline float tick(float x) { z += a * ((double) x - z); return (float) z; }
};
struct OnePoleHP {
    double a = 0.99, x1 = 0.0, y1 = 0.0;
    void set(double fc, double fs) { a = std::exp(-2.0 * kPi * fc / fs); }
    void reset() { x1 = y1 = 0.0; }
    inline float tick(float x) { const double y = a * (y1 + (double) x - x1); x1 = x; y1 = y; return (float) y; }
};

// One triode-style gain stage: smooth asymmetric saturation (even + odd harmonics),
// grid-leak style level-dependent gain, coupling cap and Miller roll-off.
struct TubeStage {
    double g = 1.0, gT = 1.0, c = 0.1, norm = 1.0 / 1.1, comp = 0.3, env = 0.0;
    double ka = 0.01, kr = 0.001, kg = 0.01;
    OnePoleHP hp; OnePoleLP lp;
    void reset() { env = 0.0; g = gT; hp.reset(); lp.reset(); }
    inline float tick(float x) {
        g += kg * (gT - g);
        const double u = (double) x * g / (1.0 + comp * env);
        const double t = std::tanh(u);
        const double y = (t + c * t * t) * norm;
        const double a = std::fabs(y);
        env += (a > env ? ka : kr) * (a - env);
        return lp.tick(hp.tick((float) y));
    }
};

// Push-pull power section: saturates and "sags" - the louder it plays the more
// the supply droops, which squashes and blooms the note like a real amp.
struct PowerAmp {
    double pg = 1.0, pgT = 1.0, sagK = 0.0, env = 0.0, ka = 0.01, kr = 0.001, kg = 0.01;
    void reset() { env = 0.0; pg = pgT; }
    inline float tick(float x) {
        pg += kg * (pgT - pg);
        const double v = (double) x * pg / (1.0 + sagK * env);
        const double y = std::tanh(v);
        const double e = y * y;
        env += (e > env ? ka : kr) * (e - env);
        return (float) y;
    }
};

// Small stereo Schroeder/Freeverb-style room with pre-delay and a band-limited
// input so it stays out of the low mids.
class Reverb {
public:
    void prepare(double fs) {
        static const int ct[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int at[4] = { 556, 441, 341, 225 };
        const double sc = fs / 44100.0;
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < 8; ++i) { comb_[ch][i].buf.assign((size_t) std::max(8, (int) std::lround((ct[i] + ch * 23) * sc)), 0.f); comb_[ch][i].idx = 0; comb_[ch][i].filt = 0.f; }
            for (int i = 0; i < 4; ++i) { ap_[ch][i].buf.assign((size_t) std::max(8, (int) std::lround((at[i] + ch * 23) * sc)), 0.f); ap_[ch][i].idx = 0; }
        }
        pd_.assign((size_t) (fs * 0.03) + 4, 0.f); pdi_ = 0; pdLen_ = (int) (fs * 0.014);
        hp_.set(170.0, fs); lp_.set(5200.0, fs); hp_.reset(); lp_.reset();
    }
    void reset() {
        for (auto& ch : comb_) for (auto& c : ch) { std::fill(c.buf.begin(), c.buf.end(), 0.f); c.filt = 0.f; c.idx = 0; }
        for (auto& ch : ap_)   for (auto& a : ch) { std::fill(a.buf.begin(), a.buf.end(), 0.f); a.idx = 0; }
        std::fill(pd_.begin(), pd_.end(), 0.f); pdi_ = 0; hp_.reset(); lp_.reset();
    }
    void setDecay(float d) { fb_ = 0.70f + 0.26f * std::max(0.f, std::min(1.f, d)); }
    void setTone(float tn) { damp_ = 0.75f - 0.65f * std::max(0.f, std::min(1.f, tn)); }
    inline void tick(float in, float& wl, float& wr) {
        const int P = (int) pd_.size();
        pd_[(size_t) pdi_] = in;
        int rd = pdi_ - pdLen_; if (rd < 0) rd += P;
        const float x = lp_.tick(hp_.tick(pd_[(size_t) rd]));
        pdi_ = (pdi_ + 1) % P;
        const float inp = x * 0.015f + 1.0e-18f;
        float o[2];
        for (int ch = 0; ch < 2; ++ch) {
            float s = 0.f;
            for (auto& c : comb_[ch]) {
                const float y = c.buf[(size_t) c.idx];
                c.filt = y * (1.f - damp_) + c.filt * damp_;
                c.buf[(size_t) c.idx] = inp + c.filt * fb_;
                if (++c.idx >= (int) c.buf.size()) c.idx = 0;
                s += y;
            }
            for (auto& a : ap_[ch]) {
                const float b = a.buf[(size_t) a.idx];
                const float y = -s + b;
                a.buf[(size_t) a.idx] = s + b * 0.5f;
                if (++a.idx >= (int) a.buf.size()) a.idx = 0;
                s = y;
            }
            o[ch] = s;
        }
        wl = o[0]; wr = o[1];
    }
private:
    struct Comb { std::vector<float> buf; int idx = 0; float filt = 0.f; };
    struct Allp { std::vector<float> buf; int idx = 0; };
    Comb comb_[2][8]; Allp ap_[2][4];
    std::vector<float> pd_; int pdi_ = 0, pdLen_ = 1;
    OnePoleHP hp_; OnePoleLP lp_;
    float fb_ = 0.86f, damp_ = 0.35f;
};
} // namespace detail

// ─── Pedal metadata ────────────────────────────────────────────────────────
enum class PedalKind {
    DS1, OD1, SD1, TS9, Klon, Rat, Muff, FuzzFace,
    Comp, EQ5, Chorus, Flanger, Phaser, Tremolo, Delay,
    BD2, OCD, ToneBender, Octavia, Boost, Wah, EnvFilter, Univibe, Tape, Reverb, NoiseGate, Octaver, HarmTrem, HM2
};

struct PedalKnobDef {
    const char* name;
    const char* label;
    float def, lo, hi;
};

struct PedalMeta {
    const char* key;
    const char* display;
    const char* brand;
    PedalKind kind;
    std::vector<PedalKnobDef> knobs;
};

inline const std::vector<PedalMeta>& pedalMetas() {
    static const std::vector<PedalMeta> m = {
        {"ds1","DS-1","BOSS",PedalKind::DS1,{{"dist","DIST",.45f,0,1},{"tone","TONE",.55f,0,1},{"level","LEVEL",.55f,0,1}}},
        {"od1","OD-1","BOSS",PedalKind::OD1,{{"drive","DRIVE",.35f,0,1},{"tone","TONE",.55f,0,1},{"level","LEVEL",.55f,0,1}}},
        {"sd1","SD-1","BOSS",PedalKind::SD1,{{"drive","DRIVE",.40f,0,1},{"tone","TONE",.55f,0,1},{"level","LEVEL",.55f,0,1}}},
        {"ts9","TS9","Ibanez",PedalKind::TS9,{{"drive","DRIVE",.40f,0,1},{"tone","TONE",.50f,0,1},{"level","LEVEL",.50f,0,1}}},
        {"klon","CENTAUR","Klon",PedalKind::Klon,{{"gain","GAIN",.35f,0,1},{"treble","TREB",.50f,0,1},{"level","OUT",.50f,0,1}}},
        {"rat","RAT","Pro Co",PedalKind::Rat,{{"dist","DIST",.50f,0,1},{"filter","FILT",.35f,0,1},{"level","VOL",.50f,0,1}}},
        {"muff","BIG MUFF","E-H",PedalKind::Muff,{{"sustain","SUST",.55f,0,1},{"tone","TONE",.50f,0,1},{"level","VOL",.45f,0,1}}},
        {"fuzzface","FUZZ FACE","Dunlop",PedalKind::FuzzFace,{{"fuzz","FUZZ",.60f,0,1},{"level","VOL",.50f,0,1}}},
        {"cs3","CS-3","BOSS",PedalKind::Comp,{{"level","LEVEL",.50f,0,1},{"tone","TONE",.50f,0,1},{"attack","ATK",.30f,0,1},{"sustain","SUST",.50f,0,1}}},
        {"dyna","DYNA COMP","MXR",PedalKind::Comp,{{"level","OUT",.50f,0,1},{"sustain","SENS",.50f,0,1}}},
        {"ge7","GE-7","BOSS",PedalKind::EQ5,{{"b1","100",0,-15,15},{"b2","400",0,-15,15},{"b3","800",0,-15,15},{"b4","1.6k",0,-15,15},{"b5","3.2k",0,-15,15},{"level","LEVEL",.50f,0,1}}},
        {"ch1","CH-1","BOSS",PedalKind::Chorus,{{"rate","RATE",.30f,0,1},{"depth","DEPTH",.50f,0,1},{"level","E.LVL",.50f,0,1},{"tone","EQ",.50f,0,1}}},
        {"bf2","BF-2","BOSS",PedalKind::Flanger,{{"manual","MAN",.40f,0,1},{"depth","DEPTH",.50f,0,1},{"rate","RATE",.30f,0,1},{"res","RES",.40f,0,1}}},
        {"phase90","PHASE 90","MXR",PedalKind::Phaser,{{"rate","SPEED",.35f,0,1},{"depth","DEPTH",.70f,0,1}}},
        {"tr2","TR-2","BOSS",PedalKind::Tremolo,{{"rate","RATE",.40f,0,1},{"depth","DEPTH",.60f,0,1},{"wave","WAVE",.20f,0,1}}},
        {"dd3","DD-3","BOSS",PedalKind::Delay,{{"level","E.LVL",.35f,0,1},{"fback","F.BK",.35f,0,1},{"time","D.TIME",320,60,800}}},
        {"bd2","BD-2","BOSS",PedalKind::BD2,{{"gain","GAIN",.40f,0,1},{"tone","TONE",.50f,0,1},{"level","LEVEL",.50f,0,1}}},
        {"ocd","OCD","Fulltone",PedalKind::OCD,{{"drive","DRIVE",.40f,0,1},{"tone","TONE",.50f,0,1},{"level","VOL",.50f,0,1},{"mode","HP/LP",.30f,0,1}}},
        {"hm2","HM-2","BOSS",PedalKind::HM2,{{"dist","DIST",.60f,0,1},{"low","LOW",.60f,0,1},{"high","HIGH",.60f,0,1},{"level","LEVEL",.50f,0,1}}},
        {"tb","TONE BENDER","Sola Sound",PedalKind::ToneBender,{{"attack","ATTACK",.60f,0,1},{"tone","TONE",.50f,0,1},{"level","LEVEL",.50f,0,1}}},
        {"octavia","OCTAVIA","Roger Mayer",PedalKind::Octavia,{{"fuzz","FUZZ",.55f,0,1},{"level","VOL",.50f,0,1}}},
        {"boost","EP BOOSTER","Xotic",PedalKind::Boost,{{"boost","BOOST",.50f,0,1},{"tone","TONE",.50f,0,1}}},
        {"ns2","NS-2","BOSS",PedalKind::NoiseGate,{{"thresh","THRESH",.40f,0,1},{"decay","DECAY",.40f,0,1}}},
        {"wah","CRY BABY","Dunlop",PedalKind::Wah,{{"sweep","SWEEP",.50f,0,1},{"q","Q",.50f,0,1},{"level","LEVEL",.50f,0,1},{"auto","AUTO",0.f,0,1}}},
        {"envf","MU-TRON III","Musitronics",PedalKind::EnvFilter,{{"sens","SENS",.50f,0,1},{"range","RANGE",.50f,0,1},{"q","PEAK",.50f,0,1}}},
        {"oc2","OC-2","BOSS",PedalKind::Octaver,{{"oct1","OCT 1",.50f,0,1},{"oct2","OCT 2",.30f,0,1},{"direct","DIRECT",.60f,0,1}}},
        {"uvibe","UNI-VIBE","Shin-ei",PedalKind::Univibe,{{"rate","SPEED",.40f,0,1},{"depth","INTENS",.70f,0,1},{"mode","MODE",.70f,0,1}}},
        {"htrem","HARM TREM","Fender",PedalKind::HarmTrem,{{"rate","SPEED",.40f,0,1},{"depth","INTENS",.60f,0,1}}},
        {"tape","SPACE ECHO","Roland",PedalKind::Tape,{{"level","E.LVL",.40f,0,1},{"fback","REPEAT",.40f,0,1},{"time","TIME",300,60,800},{"wow","WOW",.30f,0,1}}},
        {"verb","HOLY GRAIL","E-H",PedalKind::Reverb,{{"mix","MIX",.30f,0,1},{"decay","DECAY",.50f,0,1},{"tone","TONE",.50f,0,1}}},
    };
    return m;
}
inline const char* categoryOf(PedalKind k) {
    switch (k) {
        case PedalKind::Boost: case PedalKind::Comp: case PedalKind::NoiseGate: return "BOOST / DYNAMICS";
        case PedalKind::OD1: case PedalKind::SD1: case PedalKind::TS9: case PedalKind::BD2: case PedalKind::Klon: case PedalKind::OCD: return "OVERDRIVE";
        case PedalKind::DS1: case PedalKind::Rat: case PedalKind::HM2: return "DISTORTION";
        case PedalKind::FuzzFace: case PedalKind::Muff: case PedalKind::ToneBender: case PedalKind::Octavia: return "FUZZ";
        case PedalKind::Wah: case PedalKind::EnvFilter: case PedalKind::Octaver: return "FILTER / PITCH";
        case PedalKind::Chorus: case PedalKind::Flanger: case PedalKind::Phaser: case PedalKind::Univibe: case PedalKind::Tremolo: case PedalKind::HarmTrem: return "MODULATION";
        case PedalKind::Delay: case PedalKind::Tape: case PedalKind::Reverb: return "DELAY / REVERB";
        case PedalKind::EQ5: return "EQ";
    }
    return "OTHER";
}
inline const std::vector<const char*>& pedalCategories() {
    static const std::vector<const char*> v = { "BOOST / DYNAMICS", "OVERDRIVE", "DISTORTION", "FUZZ", "FILTER / PITCH", "MODULATION", "DELAY / REVERB", "EQ" };
    return v;
}
inline const PedalMeta* findMeta(const std::string& key) {
    for (auto& m : pedalMetas()) if (key == m.key) return &m;
    return nullptr;
}
inline const PedalMeta* metaOf(PedalKind k) {
    for (auto& m : pedalMetas()) if (k == m.kind) return &m;
    return nullptr;
}

// ─── StompPedal ────────────────────────────────────────────────────────────
class StompPedal {
public:
    StompPedal(PedalKind kind, double fs);

    void setParam(const std::string& name, double v) { params[name] = v; recompute(); }
    void setFs(double sr) {
        if (sr != fs) {
            fs = sr;
            if (kind == PedalKind::Delay || kind == PedalKind::Tape) dbuf_.assign((size_t)(fs * 1.0) + 8, 0.f);   // buffer length depends on fs
            if (rev_) rev_->prepare(fs);
            reset();
        }
        recompute();
    }
    void process(const float* in, float* out, int n);
    void reset();

    std::unique_ptr<StompPedal> clone() const {
        auto n = std::make_unique<StompPedal>(kind, fs);
        n->enabled = enabled;
        n->post    = post;
        n->params  = params;
        n->recompute();
        n->reset();
        return n;
    }

    const char* keyCstr() const { auto* m = metaOf(kind); return m ? m->key : ""; }

    bool enabled = true;
    bool post = false;      // false: in front of the amp   true: after amp + cab (effects-loop position)
    PedalKind kind;
    double fs;
    std::unordered_map<std::string, double> params;

private:
    void recompute();
    Biquad hp_, pre_, tone_, aux_[5];
    std::unique_ptr<ModDelay> moddelay_;
    Biquad ap_[4];
    double phase_ = 0, cg_db_ = 0, cg_lin_ = 1;
    std::vector<float> dbuf_;
    int dw_ = 0;
    double d_cur_ = -1;
    std::vector<float> sa_, sb_, sc_;
    Oversampler2 os_;
    double s1_ = 0, s2_ = 0, env_ = 0, wpos_ = 0.5, ph2_ = 0;
    int fl1_ = 1, fl2_ = 1, gh_ = 0;
    bool pos_ = false, go_ = true;
    std::unique_ptr<detail::Reverb> rev_;

    // zero-delay-feedback state-variable filter, returns the peak-normalised band-pass (used by wah / envelope filter)
    double svfBand(double xin, double fc, double k) {
        const double g = std::tan(kPi * std::min(fc, fs * 0.45) / fs);
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = xin - s2_, v1 = a1 * s1_ + a2 * v3, v2 = s2_ + a2 * s1_ + a3 * v3;
        s1_ = 2.0 * v1 - s1_; s2_ = 2.0 * v2 - s2_;
        return k * v1;
    }
};

inline StompPedal::StompPedal(PedalKind k, double sr) : kind(k), fs(sr) {
    if (auto* m = metaOf(k)) for (auto& kn : m->knobs) params[kn.name] = kn.def;
    if (k == PedalKind::Chorus || k == PedalKind::Flanger)
        moddelay_ = std::make_unique<ModDelay>(4096);
    if (k == PedalKind::Delay || k == PedalKind::Tape)
        dbuf_.assign((size_t)(fs * 1.0) + 8, 0.f);
    if (k == PedalKind::Reverb) { rev_ = std::make_unique<detail::Reverb>(); rev_->prepare(fs); }
    post = (k == PedalKind::Delay || k == PedalKind::Tape || k == PedalKind::Reverb);   // time-based effects default to the loop
    sa_.resize(4096); sb_.resize(4096); sc_.resize(4096);
    reset();
    recompute();
}

inline void StompPedal::reset() {
    hp_.reset(); pre_.reset(); tone_.reset();
    for (auto& b : aux_) b.reset();
    for (auto& b : ap_)  b.reset();
    phase_ = 0; cg_db_ = 0; cg_lin_ = 1;
    if (moddelay_) moddelay_->clear();
    if (!dbuf_.empty()) std::fill(dbuf_.begin(), dbuf_.end(), 0.f);
    dw_ = 0; d_cur_ = -1;
    os_.reset();
    s1_ = s2_ = env_ = ph2_ = 0.0; wpos_ = 0.5; fl1_ = fl2_ = 1; gh_ = 0; pos_ = false; go_ = true;
    if (rev_) rev_->reset();
}

inline void StompPedal::recompute() {
    hp_.set(bq_highpass(fs, 80, 0.707));
    auto g = [&](const char* k, double d) {
        auto it = params.find(k);
        return it != params.end() ? it->second : d;
    };
    switch (kind) {
        case PedalKind::DS1: tone_.set(bq_high_shelf(fs, 1800, (g("tone",.5)-.5)*26)); break;
        case PedalKind::OD1: tone_.set(bq_high_shelf(fs, 1100, (g("tone",.5)-.5)*16)); break;
        case PedalKind::SD1: tone_.set(bq_high_shelf(fs, 1400, (g("tone",.5)-.5)*18)); break;
        case PedalKind::TS9: pre_.set(bq_peaking(fs,720,9,0.6)); tone_.set(bq_lowpass(fs,700+g("tone",.5)*5300)); break;
        case PedalKind::Klon: pre_.set(bq_peaking(fs,1000,5,0.5)); tone_.set(bq_high_shelf(fs,3000,(g("treble",.5)-.5)*18)); break;
        case PedalKind::Rat: pre_.set(bq_highpass(fs,120)); tone_.set(bq_lowpass(fs, 6000*std::pow(0.25, g("filter",.35)))); break;
        case PedalKind::Muff: aux_[0].set(bq_lowpass(fs,1200)); aux_[1].set(bq_highpass(fs,1200)); break;
        case PedalKind::FuzzFace: aux_[0].set(bq_highpass(fs,40)); tone_.set(bq_lowpass(fs,5500)); break;
        case PedalKind::Comp:
            if (params.find("tone") != params.end())
                tone_.set(bq_high_shelf(fs,3000,(g("tone",.5)-.5)*12));
            break;
        case PedalKind::EQ5: {
            static const char* k[5] = {"b1","b2","b3","b4","b5"};
            static const double f[5] = {100,400,800,1600,3200};
            for (int i = 0; i < 5; ++i) aux_[i].set(bq_peaking(fs, f[i], g(k[i],0), 1.1));
            break;
        }
        case PedalKind::Chorus: tone_.set(bq_lowpass(fs,2000+g("tone",.5)*6000)); break;
        case PedalKind::Delay:  tone_.set(bq_lowpass(fs,4500)); break;
        case PedalKind::BD2:    pre_.set(bq_peaking(fs,950,5,0.6)); tone_.set(bq_lowpass(fs,1800+g("tone",.5)*6200)); break;
        case PedalKind::OCD:    pre_.set(bq_highpass(fs,60+g("mode",.3)*240)); tone_.set(bq_lowpass(fs,1600+g("tone",.5)*6400)); break;
        case PedalKind::ToneBender: aux_[0].set(bq_highpass(fs,70)); tone_.set(bq_lowpass(fs,2500+g("tone",.5)*4500)); break;
        case PedalKind::Octavia: pre_.set(bq_highpass(fs,150)); aux_[0].set(bq_highpass(fs,80)); tone_.set(bq_lowpass(fs,5200)); break;
        case PedalKind::Boost:  pre_.set(bq_highpass(fs,50)); tone_.set(bq_high_shelf(fs,3000,(g("tone",.5)-.5)*14)); break;
        case PedalKind::HM2:    pre_.set(bq_highpass(fs,120)); aux_[0].set(bq_peaking(fs,110,(g("low",.6)-.5)*30+4,0.8));
                                aux_[1].set(bq_peaking(fs,2600,(g("high",.6)-.5)*30+6,0.8)); aux_[2].set(bq_peaking(fs,700,-9,0.6));
                                tone_.set(bq_lowpass(fs,7500)); break;
        case PedalKind::Tape:   tone_.set(bq_lowpass(fs,3400)); aux_[0].set(bq_highpass(fs,140)); break;
        case PedalKind::HarmTrem: aux_[0].set(bq_lowpass(fs,650,0.707)); aux_[1].set(bq_highpass(fs,650,0.707)); break;
        case PedalKind::Octaver: aux_[0].set(bq_lowpass(fs,700,0.707)); aux_[1].set(bq_lowpass(fs,450)); aux_[2].set(bq_lowpass(fs,260)); break;
        case PedalKind::Reverb: if (rev_) { rev_->setDecay((float)g("decay",.5)); rev_->setTone((float)g("tone",.5)); } break;
        default: break;
    }
}

inline void StompPedal::process(const float* x, float* y, int n) {
    if (n <= 0) return;
    if (!enabled) { std::memcpy(y, x, (size_t)n * sizeof(float)); return; }
    if ((int)sa_.size() < n) { sa_.resize((size_t)n); sb_.resize((size_t)n); sc_.resize((size_t)n); }   // never overrun on big blocks
    auto P = [&](const char* k) {
        auto it = params.find(k);
        return it != params.end() ? it->second : 0.5;
    };
    auto lvl = [&]() {
        auto it = params.find("level");
        return 0.15 + (it != params.end() ? it->second : 0.5) * 1.85;
    };
    switch (kind) {
        case PedalKind::DS1: {
            const double dist = P("dist");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            os_.run(sa_.data(), sb_.data(), n, [dist](float v) {
                return (float)std::tanh(std::max(-0.92, std::min(0.92, (double)v * (1.0 + dist * 60.0))) * 1.8); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sb_[(size_t)i]) * lvl());
            break; }
        case PedalKind::OD1: {
            const double dr = P("drive");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            os_.run(sa_.data(), sb_.data(), n, [dr](float v) { return (float)std::tanh((double)v * (1.0 + dr * 14.0)); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sb_[(size_t)i]) * lvl());
            break; }
        case PedalKind::SD1: {
            const double dr = P("drive");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]) * (1.0f + (float)(dr * 22.0));
            os_.run(sa_.data(), sb_.data(), n, [](float v) {
                return (v >= 0) ? (float)std::tanh(v) : (float)(std::tanh(v * 1.35f) * 0.85f); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sb_[(size_t)i]) * lvl());
            break; }
        case PedalKind::TS9: {
            const double dr = P("drive");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = pre_.tick(sa_[(size_t)i]);
            os_.run(sb_.data(), sc_.data(), n, [dr](float v) {
                return (float)(0.85 * std::tanh((double)v * (1.0 + dr * 35.0)) + 0.15 * (double)v); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl());
            break; }
        case PedalKind::Klon: {
            const double g = P("gain");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = pre_.tick(sa_[(size_t)i]);
            os_.run(sb_.data(), sc_.data(), n, [g](float v) { return (float)std::tanh((double)v * (1.0 + g * 25.0)); });
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = sa_[(size_t)i] * (float)(1.0 - g * 0.6) + sc_[(size_t)i] * 0.8f;
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sb_[(size_t)i]) * lvl());
            break; }
        case PedalKind::Rat: {
            const double dist = P("dist");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = pre_.tick(sa_[(size_t)i]) * (1.0f + (float)(dist * 120.0));
            os_.run(sb_.data(), sc_.data(), n, [](float v) {          // diode-style soft knee instead of a hard clip
                const float u = v / 0.7f;
                return 0.7f * u / std::cbrt(1.0f + std::fabs(u) * u * u); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl());
            break; }
        case PedalKind::Muff: {
            const double sus = P("sustain"), tn = P("tone");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]) * (1.0f + (float)(sus * 30.0));
            os_.run(sa_.data(), sb_.data(), n, [](float v) { return (float)std::tanh(std::tanh((double)v) * 6.0); });
            for (int i = 0; i < n; ++i) {
                const float lo = aux_[0].tick(sb_[(size_t)i]), hi = aux_[1].tick(sb_[(size_t)i]);
                y[i] = (float)(((1.0 - tn) * lo + tn * hi) * 0.7 * lvl());
            }
            break; }
        case PedalKind::FuzzFace: {
            const double fz = P("fuzz");
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]) * (1.0f + (float)(fz * 90.0)) + (float)(0.15 * fz);
            os_.run(sa_.data(), sb_.data(), n, [](float s) {
                return (s >= 0) ? (float)std::tanh(s * 0.8f) : (float)(std::tanh(s * 2.2f) * 0.8f); });
            for (int i = 0; i < n; ++i) sc_[(size_t)i] = aux_[0].tick(sb_[(size_t)i]);
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl());
            break; }
        case PedalKind::Comp: {
            const double sus = P("sustain");
            double peak = 0.0;
            for (int i = 0; i < n; ++i) peak = std::max(peak, (double)std::fabs(x[i]));
            const double env = 20.0 * std::log10(peak + 1e-9);
            const double thr = -6.0 - sus * 36.0, ratio = 2.0 + sus * 10.0;
            const double target = -std::max(0.0, env - thr) * (1.0 - 1.0 / ratio);
            const double atk = P("attack");
            const double tau = (target < cg_db_) ? (0.002 + atk * 0.05) : 0.25;
            cg_db_ += (target - cg_db_) * (1.0 - std::exp(-(n / fs) / tau));
            const double makeup = std::min(15.0, -thr * (1.0 - 1.0 / ratio) * 0.45);
            const double newg   = std::pow(10.0, (cg_db_ + makeup) / 20.0);
            const double step   = (newg - cg_lin_) / std::max(1, n);
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = (float)(x[i] * (cg_lin_ + step * i));
            cg_lin_ = newg;
            if (params.find("tone") != params.end())
                for (int i = 0; i < n; ++i) y[i] = tone_.tick(sa_[(size_t)i]);
            else
                std::memcpy(y, sa_.data(), (size_t)n * sizeof(float));
            const double outlvl = 0.4 + (double)P("level") * 1.2;
            for (int i = 0; i < n; ++i) y[i] = (float)(y[i] * outlvl);
            break; }
        case PedalKind::EQ5: {
            std::memcpy(sa_.data(), x, (size_t)n * sizeof(float));
            for (int b = 0; b < 5; ++b)
                for (int i = 0; i < n; ++i) sa_[(size_t)i] = aux_[b].tick(sa_[(size_t)i]);
            const double lv = P("level");
            for (int i = 0; i < n; ++i) y[i] = (float)(sa_[(size_t)i] * lv * 2.0);
            break; }
        case PedalKind::Chorus: {
            const double rate = 0.1 + P("rate") * 6.0, depth = P("depth");
            float* d = sc_.data();
            for (int i = 0; i < n; ++i) {
                double lfo = std::sin(phase_ + 2 * kPi * rate * i / fs);
                d[(size_t)i] = (float)((0.012 + (0.002 + depth * 0.006) * lfo) * fs);
            }
            phase_ = std::fmod(phase_ + 2 * kPi * rate * n / fs, 2 * kPi);
            moddelay_->process(x, d, 0.f, sa_.data(), n);
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = tone_.tick(sa_[(size_t)i]);
            const double lv = P("level");
            for (int i = 0; i < n; ++i) y[i] = (float)(x[i] * (1.0 - 0.5 * lv) + sb_[(size_t)i] * lv);
            break; }
        case PedalKind::Flanger: {
            const double rate = 0.05 + P("rate") * 3.0, man = P("manual"), dep = P("depth"), res = P("res");
            const double base = (0.0006 + man * 0.005) * fs;
            float* d = sc_.data();
            for (int i = 0; i < n; ++i) {
                double lfo = std::sin(phase_ + 2 * kPi * rate * i / fs);
                d[(size_t)i] = (float)(base * (1.0 + 0.8 * dep * lfo));
            }
            phase_ = std::fmod(phase_ + 2 * kPi * rate * n / fs, 2 * kPi);
            moddelay_->process(x, d, (float)(res * 0.85), sa_.data(), n);
            for (int i = 0; i < n; ++i)
                y[i] = (float)((x[i] + sa_[(size_t)i]) * 0.6 * (1.0 - 0.4 * res));
            break; }
        case PedalKind::Phaser: {
            const double rate = 0.05 + P("rate") * 5.0, dep = P("depth");
            const double lfo = 0.5 * (1.0 + std::sin(phase_ + 2 * kPi * rate * (n / 2.0) / fs));
            phase_ = std::fmod(phase_ + 2 * kPi * rate * n / fs, 2 * kPi);
            const double f = 250.0 * std::pow(2500.0 / 250.0, lfo * (0.3 + 0.7 * dep));
            const double t = std::tan(kPi * std::min(f, fs * 0.45) / fs);
            const double a = (t - 1.0) / (t + 1.0);
            BqCoeffs c;
            c.b[0] = a; c.b[1] = 1.0; c.b[2] = 0.0;
            c.a[0] = 1.0; c.a[1] = a; c.a[2] = 0.0;
            std::memcpy(sa_.data(), x, (size_t)n * sizeof(float));
            for (int k = 0; k < 4; ++k) {
                for (int i = 0; i < n; ++i) {
                    double yy = c.b[0] * sa_[(size_t)i] + ap_[k].z1;
                    ap_[k].z1 = c.b[1] * sa_[(size_t)i] - c.a[1] * yy;
                    ap_[k].z2 = 0;
                    sa_[(size_t)i] = (float)yy;
                }
            }
            for (int i = 0; i < n; ++i) y[i] = 0.5f * (x[i] + sa_[(size_t)i]);
            break; }
        case PedalKind::Tremolo: {
            const double rate = 0.5 + P("rate") * 11.5, dep = P("depth"), w = P("wave");
            const double k = 1.0 + w * 6.0, kt = std::tanh(k);
            for (int i = 0; i < n; ++i) {
                double s = std::sin(phase_ + 2 * kPi * rate * i / fs);
                double lfo = 0.5 * (1.0 + std::tanh(s * k) / kt);
                y[i] = (float)(x[i] * (1.0 - dep * lfo));
            }
            phase_ = std::fmod(phase_ + 2 * kPi * rate * n / fs, 2 * kPi);
            break; }
        case PedalKind::Delay: {
            const int M = (int)dbuf_.size();
            double target = fs * P("time") / 1000.0;
            target = std::max((double)(n + 2), std::min((double)(M - n - 2), target));
            if (d_cur_ < 0) d_cur_ = target;
            const double nd = d_cur_ + std::max(-0.25 * n, std::min(0.25 * n, target - d_cur_));
            float* d = sc_.data();
            for (int i = 0; i < n; ++i) d[(size_t)i] = (float)(d_cur_ + (nd - d_cur_) * i / n);
            d_cur_ = nd;
            for (int i = 0; i < n; ++i) {
                double pos = dw_ + i - d[(size_t)i];
                int i0 = (int) std::floor(pos);
                double fr = pos - i0;
                int a = ((i0 % M) + M) % M;
                int b = ((i0 + 1) % M + M) % M;
                sa_[(size_t)i] = dbuf_[(size_t)a] * (1.f - (float)fr) + dbuf_[(size_t)b] * (float)fr;
            }
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = tone_.tick(sa_[(size_t)i]);
            for (int i = 0; i < n; ++i) {
                int idx = (dw_ + i) % M;
                dbuf_[(size_t)idx] = x[i] + (float)(P("fback") * 0.9) * sb_[(size_t)i];
            }
            dw_ = (dw_ + n) % M;
            for (int i = 0; i < n; ++i) y[i] = x[i] + (float)P("level") * sb_[(size_t)i];
            break; }

        case PedalKind::BD2: {
            const double g = 1.0 + P("gain") * 32.0;
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = pre_.tick(sa_[(size_t)i]);
            os_.run(sb_.data(), sc_.data(), n, [g](float v) { const double t = std::tanh((double)v * g); return (float)((t + 0.12 * t * t) / 1.12); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl());
            break; }
        case PedalKind::OCD: {
            const double g = 1.0 + P("drive") * 55.0;
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            for (int i = 0; i < n; ++i) sb_[(size_t)i] = pre_.tick(sa_[(size_t)i]);
            os_.run(sb_.data(), sc_.data(), n, [g](float v) { const double u = (double)v * g; return (float)(0.9 * (u >= 0 ? std::tanh(u) : std::tanh(u * 0.75) * 1.12)); });
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl());
            break; }
        case PedalKind::HM2: {
            const double G = 1.0 + P("dist") * 70.0;
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = pre_.tick(x[i]);
            os_.run(sa_.data(), sb_.data(), n, [G](float v) { return (float)std::tanh(std::tanh((double)v * G * 0.35) * 7.0); });
            for (int i = 0; i < n; ++i) {
                float s = aux_[0].tick(sb_[(size_t)i]); s = aux_[1].tick(s); s = aux_[2].tick(s);
                y[i] = (float)(tone_.tick(s) * lvl() * 0.6);
            }
            break; }
        case PedalKind::ToneBender: {
            const double G = 1.0 + P("attack") * 140.0;
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = hp_.tick(x[i]);
            os_.run(sa_.data(), sb_.data(), n, [G](float v) {
                auto f = [](double s) { return s >= 0 ? std::tanh(s * 1.2) : std::tanh(s * 3.5) * 0.9; };
                return (float)(f((double)v * G - 0.15) - f(-0.15)); });      // bias offset gives the gated, sputtery decay
            for (int i = 0; i < n; ++i) sc_[(size_t)i] = aux_[0].tick(sb_[(size_t)i]);
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl() * 0.8);
            break; }
        case PedalKind::Octavia: {
            const double G = 1.0 + P("fuzz") * 45.0;
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = pre_.tick(x[i]);
            os_.run(sa_.data(), sb_.data(), n, [G](float v) { const double t = std::tanh((double)v * G); return (float)(0.45 * t + 0.75 * std::fabs(t)); });   // full-wave rectified = octave up
            for (int i = 0; i < n; ++i) sc_[(size_t)i] = aux_[0].tick(sb_[(size_t)i]);
            for (int i = 0; i < n; ++i) y[i] = (float)(tone_.tick(sc_[(size_t)i]) * lvl() * 0.7);
            break; }
        case PedalKind::Boost: {
            const double g = std::pow(10.0, P("boost") * 22.0 / 20.0);
            for (int i = 0; i < n; ++i) sa_[(size_t)i] = (float)(std::tanh((double)pre_.tick(x[i]) * g * 0.6) / 0.6);
            for (int i = 0; i < n; ++i) y[i] = tone_.tick(sa_[(size_t)i]);
            break; }
        case PedalKind::NoiseGate: {
            const double thrO = std::pow(10.0, (-80.0 + P("thresh") * 55.0) / 20.0), thrC = thrO * 0.6;
            const double ka = 1.0 - std::exp(-1.0 / (fs * 0.0008)), kr = 1.0 - std::exp(-1.0 / (fs * 0.03));
            const double ko = 1.0 - std::exp(-1.0 / (fs * 0.002)), kc = 1.0 - std::exp(-1.0 / (fs * (0.02 + P("decay") * 0.4)));
            const int holdN = (int)(fs * 0.05);
            for (int i = 0; i < n; ++i) {
                const double a = std::fabs((double)x[i]);
                env_ += (a > env_ ? ka : kr) * (a - env_);
                if (!go_) { if (env_ > thrO) { go_ = true; gh_ = 0; } }
                else if (env_ < thrC) { if (++gh_ > holdN) go_ = false; }
                else gh_ = 0;
                const double tgt = go_ ? 1.0 : 0.0;
                cg_lin_ += (tgt > cg_lin_ ? ko : kc) * (tgt - cg_lin_);
                y[i] = (float)(x[i] * cg_lin_);
            }
            break; }
        case PedalKind::Wah:
        case PedalKind::EnvFilter: {
            const bool wah = (kind == PedalKind::Wah);
            const double q = P("q"), kd = 1.0 / (wah ? 2.0 + q * 8.0 : 1.5 + q * 7.0);
            const double atk = 1.0 - std::exp(-1.0 / (fs * 0.004)), rel = 1.0 - std::exp(-1.0 / (fs * (wah ? 0.15 : 0.12)));
            const double sm = 1.0 - std::exp(-1.0 / (fs * 0.012));
            const double sweep = wah ? P("sweep") : 0.0, autoAmt = wah ? P("auto") : 0.0;
            const double sens = wah ? 0.0 : P("sens"), hiRatio = wah ? 6.3 : 4.0 + P("range") * 14.0, lo = wah ? 350.0 : 220.0;
            const double lv = wah ? P("level") : 0.5;
            for (int i = 0; i < n; ++i) {
                const double a = std::fabs((double)x[i]);
                env_ += (a > env_ ? atk : rel) * (a - env_);
                double target = wah ? sweep + autoAmt * (1.0 - sweep) * std::min(1.0, env_ * 8.0)
                                    : std::min(1.0, env_ * (2.0 + sens * 30.0));
                target = std::max(0.0, std::min(1.0, target));
                wpos_ += sm * (target - wpos_);
                const double band = svfBand((double)x[i], lo * std::pow(hiRatio, wpos_), kd);
                y[i] = (float)((band * 2.5 + 0.12 * (double)x[i]) * (0.2 + lv * 1.6));
            }
            break; }
        case PedalKind::Octaver: {
            const double o1 = P("oct1"), o2 = P("oct2"), dr = P("direct");
            const double ka = 1.0 - std::exp(-1.0 / (fs * 0.003)), kr = 1.0 - std::exp(-1.0 / (fs * 0.08));
            for (int i = 0; i < n; ++i) {
                const float xl = aux_[0].tick(x[i]);
                const double a = std::fabs((double)xl);
                env_ += (a > env_ ? ka : kr) * (a - env_);
                const float th = (float)std::max(0.0008, 0.15 * env_);               // schmitt trigger: ignore tiny wobble
                if (!pos_ && xl > th) { pos_ = true; fl1_ = -fl1_; if (fl1_ > 0) fl2_ = -fl2_; }
                else if (pos_ && xl < -th) pos_ = false;
                const float s1 = aux_[1].tick(xl * (float)fl1_), s2 = aux_[2].tick(xl * (float)fl2_);
                y[i] = (float)(dr * x[i] + o1 * 2.0 * s1 + o2 * 2.0 * s2);
            }
            break; }
        case PedalKind::Univibe: {
            const double rate = 0.2 + P("rate") * 7.0, dep = P("depth"), mode = P("mode");
            static const double fr[4] = { 1.0, 1.9, 3.3, 5.5 };
            const double w = 2.0 * kPi * rate / fs, span = 1.0 + dep * 7.0, wetG = 1.0 - 0.5 * mode, dryG = 0.5 * mode;
            for (int i = 0; i < n; ++i) {
                const double l = std::pow(0.5 + 0.5 * std::sin(phase_), 1.4);       // lamp-like, slightly lopsided sweep
                phase_ += w; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                double s = x[i];
                for (int k = 0; k < 4; ++k) {
                    const double f = std::min(180.0 * fr[k] * std::pow(span, l), fs * 0.45);
                    const double tt = std::tan(kPi * f / fs), a = (tt - 1.0) / (tt + 1.0);
                    const double yy = a * s + ap_[k].z1;
                    ap_[k].z1 = s - a * yy;
                    s = yy;
                }
                y[i] = (float)(s * wetG + (double)x[i] * dryG) * 1.1f;
            }
            break; }
        case PedalKind::HarmTrem: {
            const double rate = 0.8 + P("rate") * 9.0, dep = P("depth"), w = 2.0 * kPi * rate / fs;
            for (int i = 0; i < n; ++i) {
                const double s = std::sin(phase_); phase_ += w; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                const float lo = aux_[0].tick(x[i]), hi = aux_[1].tick(x[i]);       // bands swing in opposite phase
                y[i] = (float)((lo * (1.0 - dep * 0.5 * (1.0 + s)) + hi * (1.0 - dep * 0.5 * (1.0 - s))) * (1.0 + 0.5 * dep));
            }
            break; }
        case PedalKind::Tape: {
            const int M = (int)dbuf_.size();
            const double target = std::max(64.0, std::min((double)(M - 8), fs * P("time") / 1000.0));
            if (d_cur_ < 0) d_cur_ = target;
            const double ks = 1.0 - std::exp(-1.0 / (fs * 0.12));                     // time knob glides like a real tape head
            const double wow = P("wow"), fb = P("fback"), lev = P("level");
            for (int i = 0; i < n; ++i) {
                d_cur_ += ks * (target - d_cur_);
                phase_ += 2.0 * kPi * 0.7 / fs; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                ph2_   += 2.0 * kPi * 6.3 / fs; if (ph2_   > 2.0 * kPi) ph2_   -= 2.0 * kPi;
                const double d = d_cur_ + fs * 0.0012 * wow * (std::sin(phase_) + 0.3 * std::sin(ph2_));
                double pos = (double)dw_ - d; while (pos < 0) pos += M;
                const int i0 = (int)pos, i1 = (i0 + 1) % M; const float fr = (float)(pos - i0);
                const float r = dbuf_[(size_t)i0] * (1.f - fr) + dbuf_[(size_t)i1] * fr;
                const float f = aux_[0].tick(tone_.tick(r));
                const float sat = (float)std::tanh((double)f * 1.6) / 1.6f;
                dbuf_[(size_t)dw_] = x[i] + (float)(fb * 0.92) * sat;
                dw_ = (dw_ + 1) % M;
                y[i] = x[i] + (float)(lev * 1.3) * f;
            }
            break; }
        case PedalKind::Reverb: {
            const float mix = (float)P("mix");
            for (int i = 0; i < n; ++i) {
                float wl = 0.f, wr = 0.f; if (rev_) rev_->tick(x[i], wl, wr);
                y[i] = x[i] + 0.5f * (wl + wr) * mix * 3.0f;
            }
            break; }
    }
}

// ─── PedalBoard ────────────────────────────────────────────────────────────
class PedalBoard {
public:
    explicit PedalBoard(double sr = 44100.0) : fs(sr), scratch_(4096, 0.f) {}
    PedalBoard(const PedalBoard& o) : fs(o.fs), scratch_(o.scratch_) {
        for (auto& p : o.pedals) pedals.push_back(p->clone());
    }
    PedalBoard& operator=(const PedalBoard& o) {
        if (this == &o) return *this;
        fs = o.fs;
        scratch_ = o.scratch_;
        pedals.clear();
        for (auto& p : o.pedals) pedals.push_back(p->clone());
        return *this;
    }
    PedalBoard(PedalBoard&&) noexcept = default;
    PedalBoard& operator=(PedalBoard&&) noexcept = default;
    ~PedalBoard() = default;

    void setFs(double sr) {
        fs = sr;
        for (auto& p : pedals) p->setFs(sr);
        if ((int)scratch_.size() < 4096) scratch_.resize(4096);
    }
    static constexpr int kMaxPedals = 12;
    StompPedal* add(PedalKind k) {
        if ((int)pedals.size() >= kMaxPedals) return nullptr;
        pedals.push_back(std::make_unique<StompPedal>(k, fs));
        return pedals.back().get();
    }
    void removeAt(int i) {
        if (i >= 0 && i < (int)pedals.size()) pedals.erase(pedals.begin() + i);
    }
    void move(int from, int to) {
        if (from < 0 || to < 0 ||
            from >= (int)pedals.size() || to >= (int)pedals.size() ||
            from == to) return;
        auto p = std::move(pedals[(size_t)from]);
        pedals.erase(pedals.begin() + from);
        pedals.insert(pedals.begin() + to, std::move(p));
    }
    void clear() { pedals.clear(); }
    void reset() { for (auto& p : pedals) p->reset(); }

    bool hasPost() const { for (auto& p : pedals) if (p->post) return true; return false; }

    // post = false: the pedals in front of the amp   post = true: the ones in the effects-loop position
    void process(const float* in, float* out, int n, bool post = false) {
        if (n > (int)scratch_.size()) scratch_.resize((size_t)n);
        std::memcpy(out, in, (size_t)n * sizeof(float));
        for (auto& p : pedals) {
            if (p->post != post) continue;
            p->process(out, scratch_.data(), n);
            std::memcpy(out, scratch_.data(), (size_t)n * sizeof(float));
        }
    }

    std::vector<std::unique_ptr<StompPedal>> pedals;
    double fs;
private:
    std::vector<float> scratch_;
};

// ─── Amp voicings ──────────────────────────────────────────────────────────
// Each voicing sets how the cascaded stages share the gain, how hard each stage
// is coupled/filtered, and the colour of the (pre-final-stage) tone stack.
struct EqSpec { int type; double f, g, q; };      // 0 none · 1 peak · 2 low shelf · 3 high shelf
struct Voicing {
    const char* name;
    double w[3];          // share of the total gain (dB) taken by each stage
    double comp[3];       // envelope-sensitive compression per stage
    double lp[3];         // Miller-style low-pass after each stage (Hz)
    double hp[3];         // coupling-cap high-pass after each stage (Hz)
    double asym[3];       // base even-harmonic asymmetry per stage
    double gainBaseDb, gainRangeDb, tightMaxHz, outTrim;
    EqSpec eq[3];
};

inline const std::vector<Voicing>& voicings() {
    static const std::vector<Voicing> v = {
        { "American Clean",   {0.55, 0.45, 0.00}, {0.10, 0.18, 0.00}, {11000, 9500, 8000}, {40, 48, 55}, {0.08, 0.05, 0.02},
          2.0, 40.0,  70.0, 2.40, { {1, 470, -3.0, 0.7}, {3, 3800,  1.5, 0.7}, {0, 0, 0, 1} } },
        { "British Crunch",   {0.38, 0.37, 0.25}, {0.18, 0.38, 0.45}, { 9500, 7800, 6500}, {45, 60, 70}, {0.14, 0.10, 0.06},
          4.0, 52.0, 130.0, 1.40, { {1, 520, -1.0, 0.9}, {1, 1700,  2.0, 0.8}, {0, 0, 0, 1} } },
        { "Chime (AC-style)", {0.45, 0.35, 0.20}, {0.22, 0.38, 0.30}, {12000,10000, 8500}, {55, 70, 85}, {0.16, 0.12, 0.08},
          3.0, 46.0,  90.0, 1.55, { {1, 1300, 2.5, 0.9}, {2, 130, -2.0, 0.7}, {3, 5500, 2.0, 0.7} } },
        { "Modern High-Gain", {0.30, 0.36, 0.34}, {0.15, 0.30, 0.35}, { 8800, 7000, 6000}, {55, 75, 95}, {0.10, 0.07, 0.04},
          8.0, 56.0, 190.0, 0.72, { {1, 650, -3.5, 0.9}, {1, 2000,  2.5, 1.1}, {2, 100, 2.0, 0.7} } },
    };
    return v;
}
inline const Voicing& voicingOf(int i) { const auto& v = voicings(); return v[(size_t) std::max(0, std::min((int) v.size() - 1, i))]; }



// ─── AmpSimProcessor ───────────────────────────────────────────────────────
//  pedals → [4x oversampled]: tight HP → stage 1 → stage 2 → tone stack (+voicing)
//  → stage 3 → power amp (sag) → cabinet (speaker + mic) → master → room reverb
class AmpSimProcessor {
public:
    AmpSimProcessor() : pedalboard(44100.0) { prepare(44100.0); }

    // Explicit copy so tracks can be duplicated.
    AmpSimProcessor(const AmpSimProcessor& o)
        : params(o.params),
          pedalboard(o.pedalboard),
          enabled(o.enabled),
          in_level(o.in_level),
          out_level(o.out_level) {
        init(o.fs_);
        gate_gain_ = o.gate_gain_;
    }
    AmpSimProcessor& operator=(const AmpSimProcessor&) = delete;
    AmpSimProcessor(AmpSimProcessor&&) noexcept = default;
    AmpSimProcessor& operator=(AmpSimProcessor&&) noexcept = default;

    void prepare(double sr) {
        if (std::abs(fs_ - sr) < 0.5 && (int) mono_.size() >= 4096) return;
        init(sr);
    }

    void recompute() { configure(params); }

    void reset() {
        tightHp_.reset();
        for (auto& b : vf_) b.reset();
        bass_.reset(); mid_.reset(); treble_.reset();
        for (auto& s : st_) s.reset();
        pa_.reset(); os_.reset();
        dc_.reset(); cabhp_.reset(); cabres_.reset(); cabbox_.reset(); cabm1_.reset(); cabm2_.reset();
        micsh_.reset(); micpx_.reset(); cablp1_.reset(); cablp2_.reset(); presence_.reset();
        rev_.reset();
        pedalboard.reset();
        gate_gain_ = 1.0; gate_env_ = 0.0; gate_open_ = true; gate_hold_ = 0;
    }

    void process(float* l, float* r, int n) {
        if (!enabled || n <= 0) return;
        const AmpParams p = params;
        if (p != applied_) configure(p);
        if (n > (int) mono_.size()) { mono_.resize((size_t) n); scratch_.resize((size_t) n); }
        if ((int) osb_.size() < 4 * n) osb_.resize((size_t) (4 * n));

        for (int i = 0; i < n; ++i) mono_[(size_t) i] = 0.5f * (l[i] + r[i]);

        double s = 0.0;
        for (int i = 0; i < n; ++i) s += (double) mono_[(size_t) i] * mono_[(size_t) i];
        in_level = (float) std::sqrt(s / n + 1e-9);

        // input gain + noise gate (per-sample envelope, hysteresis and hold; block-size independent)
        {
            const double thrO = std::pow(10.0, p.noise_gate_thresh_db / 20.0), thrC = thrO * 0.63;
            for (int i = 0; i < n; ++i) {
                const float x = mono_[(size_t) i] * p.input_gain;
                const double a = std::fabs((double) x);
                gate_env_ += (a > gate_env_ ? gka_ : gkr_) * (a - gate_env_);
                if (!gate_open_) { if (gate_env_ > thrO) { gate_open_ = true; gate_hold_ = 0; } }
                else if (gate_env_ < thrC) { if (++gate_hold_ > gholdN_) gate_open_ = false; }
                else gate_hold_ = 0;
                const double tgt = p.noise_gate_enabled ? (gate_open_ ? 1.0 : 0.0) : 1.0;
                gate_gain_ += (tgt > gate_gain_ ? gko_ : gkc_) * (tgt - gate_gain_);
                mono_[(size_t) i] = (float) (x * gate_gain_);
            }
        }

        pedalboard.process(mono_.data(), scratch_.data(), n);

        // nonlinear core at 4x
        os_.up(scratch_.data(), osb_.data(), n);
        for (int j = 0; j < 4 * n; ++j) osb_[(size_t) j] = tickCore(osb_[(size_t) j]);
        os_.down(osb_.data(), mono_.data(), n);

        // cabinet + master
        for (int i = 0; i < n; ++i) {
            float x = dc_.tick(mono_[(size_t) i]);
            x = cabhp_.tick(x);  x = cabres_.tick(x); x = cabbox_.tick(x);
            x = cabm1_.tick(x);  x = cabm2_.tick(x);
            x = micsh_.tick(x);  x = micpx_.tick(x);
            x = cablp1_.tick(x); x = cablp2_.tick(x);
            x = presence_.tick(x);
            gm_ += kgm_ * (masterT_ - gm_);
            mono_[(size_t) i] = softLimit(x * (float) gm_);
        }

        // pedals set to POST sit after the amp and cab, like an effects loop
        if (pedalboard.hasPost()) {
            pedalboard.process(mono_.data(), scratch_.data(), n, true);
            std::memcpy(mono_.data(), scratch_.data(), (size_t) n * sizeof(float));
        }

        // room reverb, stereo out, metering
        const double revT = std::max(0.0, std::min(1.0, (double) p.reverb));
        double s2 = 0.0; float pk = 0.f;
        for (int i = 0; i < n; ++i) {
            const float v = softLimit(mono_[(size_t) i]);
            rmix_ += kgm_ * (revT - rmix_);
            float outL = v, outR = v;
            if (rmix_ > 1.0e-4 || revT > 0.0) {
                float wl, wr; rev_.tick(v, wl, wr);
                const float wm = (float) (rmix_ * 2.4);
                outL = softLimit(v + wl * wm); outR = softLimit(v + wr * wm);
            }
            l[i] = outL; r[i] = outR;
            s2 += (double) outL * outL;
            pk = std::max(pk, std::max(std::fabs(outL), std::fabs(outR)));
        }
        out_level = (float) std::sqrt(s2 / n + 1e-9);
        out_peak = pk;
        if (pk >= 0.985f) clip = true;
    }

    AmpParams params;
    PedalBoard pedalboard;
    bool enabled = true;
    float in_level = 0.f, out_level = 0.f;
    float out_peak = 0.f;     // block peak of the last processed block (for the UI)
    bool  clip = false;       // latched when the output touched full scale; the UI clears it

    double sampleRate() const { return fs_; }

private:
    static float softLimit(float x) {
        const float a = std::fabs(x);
        if (a <= 0.9f) return x;
        const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
        return x < 0.f ? -y : y;
    }

    void init(double sr) {
        fs_ = sr;
        mono_.assign(4096, 0.f); scratch_.assign(4096, 0.f); osb_.assign(4 * 4096, 0.f);
        pedalboard.setFs(fs_);
        rev_.prepare(fs_);
        gka_ = 1.0 - std::exp(-1.0 / (fs_ * 0.001));
        gkr_ = 1.0 - std::exp(-1.0 / (fs_ * 0.030));
        gko_ = 1.0 - std::exp(-1.0 / (fs_ * 0.003));
        gkc_ = 1.0 - std::exp(-1.0 / (fs_ * 0.060));
        gholdN_ = (int) (fs_ * 0.05);
        kgm_ = 1.0 - std::exp(-1.0 / (fs_ * 0.015));
        configure(params);
        reset();
        gm_ = masterT_; rmix_ = std::max(0.0, std::min(1.0, (double) params.reverb));
        for (auto& st : st_) st.g = st.gT;
        pa_.pg = pa_.pgT;
    }

    void configure(const AmpParams& p) {
        applied_ = p;
        const double fsO = fs_ * 4.0;
        const Voicing& v = voicingOf(p.voice);
        const double drive = std::max(0.0, std::min(1.0, (double) p.drive));
        const double tight = std::max(0.0, std::min(1.0, (double) p.tight));
        const double sag   = std::max(0.0, std::min(1.0, (double) p.sag));
        const double totalDb = v.gainBaseDb + v.gainRangeDb * drive;
        const double kA = 1.0 - std::exp(-1.0 / (fsO * 0.004)), kR = 1.0 - std::exp(-1.0 / (fsO * 0.090)), kG = 1.0 - std::exp(-1.0 / (fsO * 0.010));
        for (int i = 0; i < 3; ++i) {
            auto& s = st_[i];
            s.gT = std::pow(10.0, totalDb * v.w[i] / 20.0);
            s.c = v.asym[i] + (double) p.asym * (0.30 - 0.08 * i);
            s.norm = 1.0 / (1.0 + std::max(0.0, s.c));
            s.comp = v.comp[i];
            s.ka = kA; s.kr = kR; s.kg = kG;
            s.hp.set(v.hp[i] + tight * (30.0 + 30.0 * i), fsO);
            s.lp.set(v.lp[i], fsO);
        }
        tightHp_.set(bq_highpass(fsO, 36.0 + tight * v.tightMaxHz, 0.707));
        for (int k = 0; k < 3; ++k) {
            const EqSpec& e = v.eq[k];
            if      (e.type == 1) vf_[k].set(bq_peaking(fsO, e.f, e.g, e.q));
            else if (e.type == 2) vf_[k].set(bq_low_shelf(fsO, e.f, e.g));
            else if (e.type == 3) vf_[k].set(bq_high_shelf(fsO, e.f, e.g));
            else                  vf_[k].set(BqCoeffs());
        }
        bass_.set(bq_low_shelf(fsO, 120, p.bass_db));
        mid_.set(bq_peaking(fsO, 800, p.mid_db, 0.8));
        treble_.set(bq_high_shelf(fsO, 2800, p.treble_db));

        pa_.pgT = 0.65 + sag * 1.5;
        pa_.sagK = sag * 1.7;
        pa_.ka = 1.0 - std::exp(-1.0 / (fsO * 0.012)); pa_.kr = 1.0 - std::exp(-1.0 / (fsO * 0.180)); pa_.kg = kG;

        // cabinet: speaker + box + mic position (all at the base rate)
        const double mic = std::max(0.0, std::min(1.0, (double) p.mic));
        dc_.set(bq_highpass(fs_, 20));
        cabhp_.set(bq_highpass(fs_, 78.0, 0.75));
        cabres_.set(bq_peaking(fs_, 115.0, p.cab_res_db, 1.1));
        cabbox_.set(bq_peaking(fs_, 390.0, -1.0, 0.9));
        cabm1_.set(bq_peaking(fs_, 2150.0, 2.0, 1.0));
        cabm2_.set(bq_peaking(fs_, 3500.0, 1.0, 1.6));
        micsh_.set(bq_high_shelf(fs_, 3000.0, (0.35 - mic) * 10.0));
        micpx_.set(bq_low_shelf(fs_, 180.0, (0.5 - mic) * 5.0));
        const double lpc = std::min((double) p.cab_lp_hz * (1.0 - 0.22 * mic), fs_ * 0.45);
        cablp1_.set(bq_lowpass(fs_, lpc, 0.62));
        cablp2_.set(bq_lowpass(fs_, lpc, 1.00));
        presence_.set(bq_high_shelf(fs_, 3500, p.presence_db));

        masterT_ = (double) p.master * v.outTrim * kOutBase / (0.75 + 0.5 * pa_.pgT);
    }

    inline float tickCore(float x) {
        float s = tightHp_.tick(x);
        s = st_[0].tick(s);
        s = st_[1].tick(s);
        s = vf_[0].tick(s); s = vf_[1].tick(s); s = vf_[2].tick(s);
        s = bass_.tick(s);  s = mid_.tick(s);   s = treble_.tick(s);
        s = st_[2].tick(s);
        return pa_.tick(s);
    }

    static constexpr double kOutBase = 1.0;
    double fs_ = 44100.0, gate_gain_ = 1.0;
    AmpParams applied_;
    detail::TubeStage st_[3];
    detail::PowerAmp pa_;
    Biquad tightHp_, vf_[3], bass_, mid_, treble_;
    Biquad dc_, cabhp_, cabres_, cabbox_, cabm1_, cabm2_, micsh_, micpx_, cablp1_, cablp2_, presence_;
    Oversampler4 os_;
    detail::Reverb rev_;
    double gate_env_ = 0.0, gka_ = 0.01, gkr_ = 0.001, gko_ = 0.01, gkc_ = 0.001;
    bool gate_open_ = true; int gate_hold_ = 0, gholdN_ = 2000;
    double gm_ = 0.6, masterT_ = 0.6, kgm_ = 0.01, rmix_ = 0.0;
    std::vector<float> mono_, scratch_, osb_;
};

// ─── Presets ───────────────────────────────────────────────────────────────
struct AmpPreset { const char* name; AmpParams p; };

inline const std::vector<AmpPreset>& presets() {
    auto P = [](float dr, float as, float bs, float md, float tr,
                float cr, float cl, float pr, float ms,
                int voice = 1, float tight = 0.3f, float sag = 0.35f, float mic = 0.3f, float rev = 0.f) {
        AmpParams p;
        p.drive = dr; p.asym = as;
        p.bass_db = bs; p.mid_db = md; p.treble_db = tr;
        p.cab_res_db = cr; p.cab_lp_hz = cl; p.presence_db = pr;
        p.master = ms;
        p.voice = voice; p.tight = tight; p.sag = sag; p.mic = mic; p.reverb = rev;
        return p;
    };
    static const std::vector<AmpPreset> v = {
        {"Clean",                        P(0.05f,0.00f, 1.0f, 0.0f, 1.0f,0.0f,6500,0.0f,0.70f, 0,0.10f,0.20f,0.25f,0.10f)},
        {"Crunch",                       P(0.35f,0.15f, 1.0f,-1.0f, 2.0f,1.0f,5500,1.0f,0.60f, 1,0.30f,0.35f,0.30f,0.10f)},
        {"Songs for the Deaf (Fuzz)",    P(0.78f,0.55f, 4.0f,-3.5f, 3.0f,4.5f,4400,3.0f,0.50f, 1,0.15f,0.70f,0.40f,0.05f)},
        {"High Gain Modern",             P(0.90f,0.20f, 2.0f,-2.0f, 1.0f,1.5f,5000,4.0f,0.45f, 3,0.55f,0.35f,0.25f,0.08f)},
        {"American Clean",               P(0.10f,0.00f, 1.5f,-1.0f, 2.5f,0.5f,6800,1.5f,0.72f, 0,0.08f,0.15f,0.20f,0.22f)},
        {"British Crunch",               P(0.52f,0.08f, 0.5f, 2.5f, 2.0f,1.2f,5600,1.5f,0.58f, 1,0.30f,0.40f,0.30f,0.08f)},
        {"Indie Jangle",                 P(0.18f,0.00f,-1.0f, 1.0f, 4.0f,0.0f,7200,2.5f,0.65f, 2,0.15f,0.30f,0.20f,0.20f)},
        {"Blues Breakup",                P(0.42f,0.06f, 2.0f, 3.0f, 0.5f,1.8f,5200,0.5f,0.62f, 0,0.12f,0.55f,0.35f,0.18f)},
        {"Stoner Wall",                  P(0.82f,0.42f, 5.0f,-1.5f,-1.0f,5.5f,3700,0.0f,0.48f, 1,0.05f,0.75f,0.45f,0.05f)},
        {"Tight Metal",                  P(0.88f,0.12f, 1.0f,-3.0f, 2.0f,0.8f,6000,4.5f,0.43f, 3,0.75f,0.30f,0.20f,0.04f)},
        {"Desert Lead",                  P(0.66f,0.24f, 2.0f, 1.5f, 2.5f,2.0f,5100,2.5f,0.50f, 1,0.25f,0.55f,0.35f,0.15f)},
        {"Shoegaze Bloom",               P(0.58f,0.16f, 1.0f, 0.5f, 3.5f,1.0f,6400,3.0f,0.52f, 2,0.20f,0.50f,0.30f,0.45f)},
        {"Garage Bite",                  P(0.70f,0.36f,-0.5f, 2.0f, 3.0f,0.5f,5400,2.0f,0.48f, 1,0.30f,0.45f,0.35f,0.08f)},
        {"Doom Velvet",                  P(0.85f,0.32f, 3.0f, 1.0f,-1.5f,3.0f,4000,0.0f,0.44f, 1,0.08f,0.80f,0.50f,0.10f)},
        {"Neon Fuzz",                    P(0.74f,0.70f,-1.0f, 1.5f, 4.5f,0.0f,5900,3.0f,0.46f, 2,0.25f,0.55f,0.30f,0.25f)},
        {"Jazz Midnight",                P(0.08f,0.00f, 2.0f, 2.0f,-2.0f,1.0f,4200,-0.5f,0.60f, 0,0.05f,0.10f,0.60f,0.20f)},
        {"Arena Lead",                   P(0.70f,0.10f, 1.0f, 2.5f, 2.5f,1.0f,6100,3.5f,0.48f, 1,0.35f,0.45f,0.25f,0.22f)},
        {"Surf Sparkle",                 P(0.14f,0.00f,-1.5f, 0.0f, 4.5f,0.0f,7600,3.5f,0.60f, 0,0.10f,0.20f,0.15f,0.50f)},
        {"Tool (Adam Jones)",            P(0.92f,0.25f, 3.5f, 1.0f, 2.0f,3.0f,5800,4.0f,0.48f, 3,0.50f,0.40f,0.30f,0.06f)},
        {"Nirvana (Grunge Fuzz)",        P(0.82f,0.65f, 4.5f,-3.0f, 3.5f,2.5f,4800,2.0f,0.48f, 1,0.15f,0.60f,0.40f,0.06f)},
        {"Foo Fighters (Wall of Sound)", P(0.75f,0.20f, 2.5f, 2.0f, 1.5f,2.0f,5400,3.0f,0.52f, 1,0.35f,0.45f,0.30f,0.08f)},
        {"Metallica (Puppet Scope)",     P(0.95f,0.15f, 4.0f,-5.0f, 4.0f,1.5f,6000,5.0f,0.42f, 3,0.80f,0.30f,0.20f,0.03f)},
        {"Pink Floyd (Singing Lead)",    P(0.60f,0.40f, 2.0f, 2.5f, 1.0f,2.0f,4600,1.5f,0.55f, 2,0.20f,0.50f,0.35f,0.35f)},
        {"Led Zeppelin (Plexi 10)",      P(0.55f,0.10f, 1.0f, 3.0f, 2.5f,1.0f,5700,2.0f,0.58f, 1,0.25f,0.55f,0.30f,0.12f)},
        {"Van Halen (Brown Sound)",      P(0.72f,0.18f, 2.0f, 3.5f, 1.5f,1.8f,5200,3.5f,0.50f, 1,0.35f,0.50f,0.30f,0.10f)},
        {"RHCP (Spanky Edge)",           P(0.22f,0.05f,-1.0f, 1.5f, 4.5f,0.5f,7000,3.5f,0.65f, 0,0.15f,0.30f,0.20f,0.12f)},
        {"Polyphia (Glass Clean)",       P(0.08f,0.00f,-2.0f,-2.0f, 5.5f,0.0f,7800,4.5f,0.68f, 0,0.10f,0.15f,0.15f,0.22f)},
        {"Black Sabbath (Doom Laney)",   P(0.86f,0.50f, 5.0f, 2.0f,-1.0f,4.0f,3900,0.5f,0.46f, 1,0.08f,0.75f,0.45f,0.06f)},
    };
    return v;
}

// ─── Cabinet sims ──────────────────────────────────────────────────────────
struct CabSim { const char* name; float res_db, lp_hz, presence_db, mic; };

inline const std::vector<CabSim>& cabSims() {
    static const std::vector<CabSim> v = {
        {"Open-back 1x12",      0.5f, 6800, 2.0f, 0.30f}, {"British 2x12",        1.5f, 5600, 1.5f, 0.30f},
        {"Vintage 4x12",        3.5f, 4700, 1.0f, 0.35f}, {"Modern 4x12",         1.0f, 6200, 4.0f, 0.20f},
        {"Dark oversized 4x12", 5.0f, 3800, 0.0f, 0.55f}, {"Tight 1x12",          0.0f, 6500, 3.0f, 0.20f},
        {"Lo-fi 1x10",          0.5f, 3400,-1.0f, 0.60f}, {"Wide 2x12",           1.0f, 6100, 2.5f, 0.30f},
        {"Jazz 1x12",           1.0f, 4200,-0.5f, 0.45f}, {"Bright 2x10",         0.0f, 7600, 3.5f, 0.15f},
        {"Fuzz 2x12",           0.0f, 5900, 3.0f, 0.30f},
    };
    return v;
}

// ─── Pedalboard rigs ───────────────────────────────────────────────────────
struct BoardSlot { const char* key; bool post; std::vector<std::pair<const char*, double>> params; };
struct BoardPreset { const char* name; std::vector<BoardSlot> slots; };

inline const std::vector<BoardPreset>& boardPresets() {
    static const std::vector<BoardPreset> v = {
        { "Blues Breakup",     { {"ts9", false, {{"drive",.30},{"tone",.45},{"level",.55}}}, {"verb", true, {{"mix",.22},{"decay",.45},{"tone",.55}}} } },
        { "Gilmour Lead",      { {"cs3", false, {{"level",.5},{"tone",.5},{"attack",.3},{"sustain",.4}}}, {"ts9", false, {{"drive",.25},{"tone",.55},{"level",.6}}},
                                 {"muff", false, {{"sustain",.55},{"tone",.42},{"level",.45}}}, {"dd3", true, {{"level",.32},{"fback",.38},{"time",440}}}, {"verb", true, {{"mix",.30},{"decay",.70},{"tone",.45}}} } },
        { "Hendrix Voodoo",    { {"wah", false, {{"sweep",.5},{"q",.55},{"level",.55},{"auto",0}}}, {"fuzzface", false, {{"fuzz",.7},{"level",.5}}}, {"uvibe", false, {{"rate",.45},{"depth",.7},{"mode",.85}}} } },
        { "Edge Dotted Delay", { {"cs3", false, {{"level",.5},{"tone",.55},{"attack",.2},{"sustain",.3}}}, {"ch1", false, {{"rate",.2},{"depth",.3},{"level",.3},{"tone",.6}}}, {"dd3", true, {{"level",.45},{"fback",.42},{"time",375}}} } },
        { "Shoegaze Wall",     { {"muff", false, {{"sustain",.65},{"tone",.4},{"level",.4}}}, {"ch1", false, {{"rate",.25},{"depth",.6},{"level",.5},{"tone",.45}}},
                                 {"tape", true, {{"level",.45},{"fback",.55},{"time",520},{"wow",.4}}}, {"verb", true, {{"mix",.6},{"decay",.92},{"tone",.4}}} } },
        { "Modern Metal Tight",{ {"ns2", false, {{"thresh",.5},{"decay",.25}}}, {"ts9", false, {{"drive",.05},{"tone",.62},{"level",.85}}}, {"ge7", false, {{"b1",-2},{"b2",0},{"b3",-4},{"b4",3},{"b5",4},{"level",.5}}} } },
        { "Swedish Chainsaw",  { {"hm2", false, {{"dist",.75},{"low",.7},{"high",.65},{"level",.5}}}, {"ns2", false, {{"thresh",.45},{"decay",.3}}} } },
        { "Stoner Fuzz",       { {"tb", false, {{"attack",.7},{"tone",.45},{"level",.5}}}, {"oc2", false, {{"oct1",.35},{"oct2",.15},{"direct",.8}}}, {"phase90", false, {{"rate",.2},{"depth",.6}}} } },
        { "Funk Envelope",     { {"dyna", false, {{"level",.55},{"sustain",.55}}}, {"envf", false, {{"sens",.55},{"range",.5},{"q",.55}}} } },
        { "Surf Reverb",       { {"tr2", false, {{"rate",.45},{"depth",.55},{"wave",.2}}}, {"verb", true, {{"mix",.65},{"decay",.6},{"tone",.7}}} } },
        { "Octave Lead",       { {"octavia", false, {{"fuzz",.6},{"level",.5}}}, {"dd3", true, {{"level",.35},{"fback",.4},{"time",360}}} } },
        { "Ambient Pad",       { {"boost", false, {{"boost",.3},{"tone",.5}}}, {"ch1", false, {{"rate",.2},{"depth",.5},{"level",.5},{"tone",.5}}},
                                 {"tape", true, {{"level",.5},{"fback",.6},{"time",600},{"wow",.5}}}, {"verb", true, {{"mix",.7},{"decay",.95},{"tone",.35}}} } },
    };
    return v;
}

// Replaces the whole board with a rig (the caller holds the audio lock). Returns the number of pedals added.
inline int loadBoardPreset(PedalBoard& pb, const BoardPreset& bp) {
    pb.clear();
    for (const auto& s : bp.slots) {
        const PedalMeta* m = findMeta(s.key);
        if (!m) continue;
        StompPedal* p = pb.add(m->kind);
        if (!p) break;
        p->post = s.post;
        for (const auto& kv : s.params) p->setParam(kv.first, kv.second);
    }
    pb.reset();
    return (int) pb.pedals.size();
}

} // namespace ampsim
