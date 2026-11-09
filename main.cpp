#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioCore.h"
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#if JUCE_MAC || JUCE_LINUX
 #include <pthread.h>
#endif
#if JUCE_MAC || JUCE_LINUX
 #include <unistd.h>
#endif
#include "Stemsplitter.h"     
#include "AmpSim.h"
#include "AmpSimPanel.h"

using namespace juce;
struct Note { double start, len; int pitch; int vel = 100; };
struct Clip {
    double start = 0, len = 0; std::vector<Note> notes; bool rec = false, mute = false; double fadeIn = 0, fadeOut = 0;
    std::shared_ptr<std::vector<float>> wav, wavR; double wavSr = 44100; std::vector<float> peaks; int peaksDone = 0;
    double offset = 0;   // beats trimmed off the start of the audio (wavR = right channel, null = mono)   
    Clip() = default;
    Clip(double s, double l, std::vector<Note> n = {}) : start(s), len(l), notes(std::move(n)) {}
};
struct EqBand { float freq, gain = 0.f, q = 0.9f; int type = 1;   // type: 0 low shelf, 1 peak, 2 high shelf
    explicit EqBand(float f = 1000.f, float q0 = 0.9f, int ty = 1) : freq(f), q(q0), type(ty) {} };
struct Biquad {   // RBJ cookbook; type 0 = low shelf, 1 = peak, 2 = high shelf
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] { 0, 0 }, z2[2] { 0, 0 };
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    void set(int type, double sr, double f, double gDb, double q) {
        f = jlimit(20.0, sr * 0.45, f); q = jmax(0.1, q);
        const double A = std::pow(10.0, gDb / 40.0), w = MathConstants<double>::twoPi * f / sr, cs = std::cos(w), sn = std::sin(w);
        double B0, B1, B2, A0, A1, A2;
        if (type == 1) { const double al = sn / (2.0 * q); B0 = 1 + al * A; B1 = -2 * cs; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * cs; A2 = 1 - al / A; }
        else { const double sA = std::sqrt(A) * sn / q;   // shelf slope follows Q (0.707 = classic shelf)
            if (type == 0) { B0 = A * ((A + 1) - (A - 1) * cs + sA); B1 = 2 * A * ((A - 1) - (A + 1) * cs); B2 = A * ((A + 1) - (A - 1) * cs - sA); A0 = (A + 1) + (A - 1) * cs + sA; A1 = -2 * ((A - 1) + (A + 1) * cs); A2 = (A + 1) + (A - 1) * cs - sA; }
            else { B0 = A * ((A + 1) + (A - 1) * cs + sA); B1 = -2 * A * ((A - 1) + (A + 1) * cs); B2 = A * ((A + 1) + (A - 1) * cs - sA); A0 = (A + 1) - (A - 1) * cs + sA; A1 = 2 * ((A - 1) - (A + 1) * cs); A2 = (A + 1) - (A - 1) * cs - sA; } }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; }
    float run(int ch, float x) { const double y = b0 * x + z1[ch]; z1[ch] = b1 * x - a1 * y + z2[ch]; z2[ch] = b2 * x - a2 * y; return (float) y; }
    double magDb(double hz, double sr) const {
        const double w = MathConstants<double>::twoPi * hz / sr, c1 = std::cos(w), c2 = std::cos(2.0 * w);
        const double num = b0 * b0 + b1 * b1 + b2 * b2 + 2.0 * (b0 * b1 + b1 * b2) * c1 + 2.0 * b0 * b2 * c2, den = 1.0 + a1 * a1 + a2 * a2 + 2.0 * (a1 + a1 * a2) * c1 + 2.0 * a2 * c2;
        return 10.0 * std::log10(jmax(1.0e-12, num / jmax(1.0e-12, den))); }
};
struct Track {
    String name; Colour colour; float vol = 0.8f, pan = 0; bool mute = false, solo = false;
    std::vector<Clip> clips; float phase[128] {}, gain[128] {}; std::atomic<float> level { 0 };
    bool audio = false, armed = false; int inCh = 0; std::atomic<bool> live[128] {}; MidiMessageCollector coll;
    std::unique_ptr<AudioPluginInstance> plugin;            
    std::unique_ptr<AudioPluginInstance> fx[4];
    std::unique_ptr<ampsim::AmpSimProcessor> ampSim;   // built-in amp / cab / pedalboard
    int  ampSimSlot = -1;                              // which fx slot (0..3) is occupied by the amp sim
    bool hasAmp(int s) const { return ampSim != nullptr && ampSimSlot == s; }
    bool slotUsed(int s) const { return s < 0 ? plugin != nullptr : (fx[s] != nullptr || hasAmp(s)); }
    String fxLabel(int s, int maxLen) const { return fx[s] ? fx[s]->getName().substring(0, maxLen) : hasAmp(s) ? String("Amp Sim").substring(0, maxLen) : String(); }
    EqBand eq[6] { EqBand(80.f, 0.71f, 0), EqBand(400.f), EqBand(2500.f), EqBand(9000.f, 0.71f, 2), EqBand(1000.f), EqBand(5000.f) }; int nEq = 4; bool eqOn = true; int icon = 0; float send = 0.f, sendS = 0.f; Biquad bq[6];   // channel EQ: low shelf, 2 peaks, high shelf
    void eqReset() { const EqBand d[6] = { EqBand(80.f, 0.71f, 0), EqBand(400.f), EqBand(2500.f), EqBand(9000.f, 0.71f, 2), EqBand(1000.f), EqBand(5000.f) }; for (int k = 0; k < 6; ++k) eq[k] = d[k]; nEq = 4; }
    void eqSave(XmlElement& e) const { e.setAttribute("eqN", nEq); for (int k = 0; k < nEq; ++k) { e.setAttribute("eqF" + String(k), (double) eq[k].freq); e.setAttribute("eqG" + String(k), (double) eq[k].gain); e.setAttribute("eqQ" + String(k), (double) eq[k].q); e.setAttribute("eqT" + String(k), eq[k].type); } }
    void eqLoad(const XmlElement& e) {
        const bool legacy = ! e.hasAttribute("eqN"); nEq = legacy ? 4 : jlimit(1, 6, e.getIntAttribute("eqN", 4));
        for (int k = 0; k < nEq; ++k) { auto& b = eq[k]; b.freq = (float) e.getDoubleAttribute("eqF" + String(k), b.freq); b.gain = (float) e.getDoubleAttribute("eqG" + String(k), 0.0);
            b.q = (float) e.getDoubleAttribute("eqQ" + String(k), b.q); b.type = jlimit(0, 2, e.getIntAttribute("eqT" + String(k), b.type)); if (legacy && b.type != 1) b.q = 0.71f; } }
    double eqDb(double hz, double sr) const { double sum = 0; for (int k = 0; k < nEq; ++k) { if (std::abs(eq[k].gain) < 0.01f) continue; Biquad b; b.set(eq[k].type, sr, eq[k].freq, eq[k].gain, eq[k].q); sum += b.magDb(hz, sr); } return sum; }              
    bool prev[128] {}; float dEnv[128] {}, dPh[128] {}, dLast = 0;   
    bool monitor = false; int preset = 0, wave = 0; float cutoff = 0.8f, atk = 0.005f, rel = 0.15f, lp = 0;
    float vel[128] {};                                       
    Track() { std::fill(vel, vel + 128, 1.f); }
};
struct Preset { const char* name; int wave; float cutoff, atk, rel; };
struct Project {
    static constexpr int kPresets = 10;
    static constexpr Preset presets[kPresets] = { { "Saw Lead", 0, 0.8f, 0.005f, 0.15f }, { "Square Bass", 1, 0.3f, 0.003f, 0.1f }, { "Sine Pad", 2, 1.f, 0.4f, 0.9f }, { "Triangle Keys", 3, 0.7f, 0.005f, 0.35f }, { "Warm Saw", 0, 0.35f, 0.3f, 0.8f },
        { "Electric Piano", 3, 0.6f, 0.004f, 0.5f }, { "Organ", 1, 0.55f, 0.01f, 0.06f }, { "Sub Bass", 2, 0.2f, 0.003f, 0.15f }, { "Strings", 0, 0.45f, 0.25f, 0.7f }, { "Drum Kit", 10, 1.f, 0.001f, 0.2f } };
    static void applyPreset(Track& t, int i) { auto& q = presets[i]; t.preset = i; t.wave = q.wave; t.cutoff = q.cutoff; t.atk = q.atk; t.rel = q.rel; }
    int recTrack = -1, recClip = -1;
    std::atomic<float*> recPtr { nullptr }; std::atomic<int> recPos { 0 }, recCap { 0 }, recCh { 0 }; std::atomic<bool> recBusy { false }, cycle { false };
    std::atomic<double> loopStart { 0 }, loopEnd { 16 }; std::atomic<float> masterLevel { 0 };
    std::atomic<float*> recNext { nullptr }; std::atomic<int> recNextFrom { 0 }, recNextCap { 0 }; std::atomic<double> recFrom { 0 };
    std::atomic<bool> counting { false }, clickRecOnly { false }; std::atomic<float> clickVol { 0.3f };
    int countBars = 0, latencyMs = 0; bool autoScroll = true, showWave = true, showNotes = true, latencyAuto = true;      
    std::atomic<int> beatsPerBar { 4 }, clickType { 0 }, clickSub { 1 }; std::atomic<bool> clickAccent { true }, masterLimit { false }, masterClip { false };   
    std::atomic<float> clickHi { 1600.f }, clickLo { 1000.f }, masterLevelL { 0 }, masterLevelR { 0 };
    int noteSnap = 3, noteLenIdx = 1, regionSnap = 2, defVel = 100, accent = 0, autoIdx = 0;          
    Track masterTrack; std::atomic<float> revRoom { 0.6f }, revDamp { 0.5f }, revReturn { 0.35f }; std::vector<std::pair<double, String>> markers; String notes;   // reverb bus, markers, project notes
                                                                                  
    double noteGrid() const { static const double g[5] = { 0.0, 1.0, 0.5, 0.25, 0.125 }; return g[jlimit(0, 4, noteSnap)]; }
    double noteLength() const { static const double l[4] = { 0.25, 0.5, 1.0, 2.0 }; return l[jlimit(0, 3, noteLenIdx)]; }
    double regionGrid() const { return regionSnap <= 0 ? 0.0 : regionSnap == 1 ? 0.5 : regionSnap == 2 ? 1.0 : (double) jmax(1, beatsPerBar.load()); }
    using Snap = std::vector<std::vector<Clip>>; std::vector<Snap> undoS, redoS;                                             
    Snap snap() { Snap s; for (auto& tr : tracks) s.push_back(tr->clips); return s; }
    void pushUndo() { ScopedLock l(lock); undoS.push_back(snap()); if (undoS.size() > 100) undoS.erase(undoS.begin()); redoS.clear(); }
    bool restore(const Snap& s) { if (s.size() != tracks.size()) return false; for (size_t i = 0; i < s.size(); ++i) tracks[i]->clips = s[i]; selClip = -1; return true; }
    void undo() { ScopedLock l(lock); if (undoS.empty() || recClip >= 0) return; auto cur = snap(); auto s = undoS.back(); undoS.pop_back(); if (restore(s)) redoS.push_back(cur); }
    void redo() { ScopedLock l(lock); if (redoS.empty() || recClip >= 0) return; auto cur = snap(); auto s = redoS.back(); redoS.pop_back(); if (restore(s)) undoS.push_back(cur); }
    std::vector<std::unique_ptr<Track>> tracks;
    std::atomic<double> bpm { 120 }, beat { 0 }; std::atomic<bool> playing { false };
    double length = 32, sr = 44100; CriticalSection lock; int selTrack = 0, selClip = -1;
    std::atomic<bool> metro { false }, rec { false }; std::atomic<float> master { 0.9f }; std::atomic<int> skipCount { 0 };
    AudioPluginFormatManager fm; KnownPluginList known;
    void addTrack(bool audio) {
        auto t = std::make_unique<Track>(); t->audio = audio; t->coll.reset(sr);
        t->name = String(audio ? "Audio " : "Inst ") + String((int) tracks.size() + 1);
        t->colour = Colour::fromHSV(std::fmod((float) tracks.size() * 0.17f, 1.f), 0.6f, 0.9f, 1.f);
        ScopedLock sl(lock); tracks.push_back(std::move(t)); selTrack = (int) tracks.size() - 1; selClip = -1;   
    }
    static constexpr int lo = 36, hi = 84;     
    Project() { masterTrack.name = "Master"; }                       
    Clip* selected() {
        if (selTrack < 0 || selTrack >= (int) tracks.size()) return nullptr;
        auto& c = tracks[(size_t) selTrack]->clips;
        return (selClip >= 0 && selClip < (int) c.size()) ? &c[(size_t) selClip] : nullptr;
    }
};
struct Engine {
    Project& p; AudioBuffer<float> tmp { 2, 4096 }; double cph = 0, cenv = 0, cf = 1000, cph2 = 0; float cgain = 1.f, cLast = 0.f; bool was = false; float inc[128] {};
    Reverb rev; AudioBuffer<float> sendBuf { 2, 4096 }; double revSr = 0; int revTail = 0;
    explicit Engine(Project& pr) : p(pr) {}
    static void eqBlock(Track& t, float* d0, float* d1, int n, double sr) {
        for (int k = 0; k < 6; ++k) { if (k < t.nEq && t.eqOn && std::abs(t.eq[k].gain) > 0.05f) { t.bq[k].set(t.eq[k].type, sr, t.eq[k].freq, t.eq[k].gain, t.eq[k].q);
                for (int i = 0; i < n; ++i) { d0[i] = t.bq[k].run(0, d0[i]); d1[i] = t.bq[k].run(1, d1[i]); } } else t.bq[k].reset(); } }
    void synth(Track& t, const bool* on, int n, double sr) {
        const float aA = 1.f - std::exp(-1.f / jmax(1.f, t.atk * (float) sr)), aR = 1.f - std::exp(-1.f / jmax(1.f, t.rel * (float) sr));
        const float fc = 80.f * std::pow(2.f, t.cutoff * 7.5f), lpA = 1.f - std::exp(-MathConstants<float>::twoPi * fc / (float) sr);
        auto* d0 = tmp.getWritePointer(0); auto* d1 = tmp.getWritePointer(1);
        for (int i = 0; i < n; ++i) {
            float s = 0;
            for (int k = 0; k < 128; ++k) {
                float& g = t.gain[k]; const float tg = on[k] ? 1.f : 0.f; g += (tg - g) * (tg > g ? aA : aR); if (g < 0.0005f) continue;
                float& ph = t.phase[k]; ph += inc[k]; ph -= std::floor(ph);
                const float w = t.wave == 0 ? 2.f * ph - 1.f : t.wave == 1 ? (ph < 0.5f ? 1.f : -1.f) : t.wave == 2 ? std::sin(MathConstants<float>::twoPi * ph) : 4.f * std::abs(ph - 0.5f) - 1.f;
                s += w * g * 0.12f * t.vel[k]; }
            t.lp += lpA * (s - t.lp); d0[i] = d1[i] = t.lp; }
    }
    uint32_t rng = 22222;
    void drums(Track& t, const bool* on, int n, double sr) {          
        for (int k = 0; k < 128; ++k) { if (on[k] && ! t.prev[k]) { t.dEnv[k] = 1.f; t.dPh[k] = 0.f; } t.prev[k] = on[k]; }
        auto* d0 = tmp.getWritePointer(0); auto* d1 = tmp.getWritePointer(1); const float fs = (float) sr, tp = MathConstants<float>::twoPi;
        for (int i = 0; i < n; ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; const float nz = (float) (rng & 0xffff) / 32768.f - 1.f, hp = nz - t.dLast; t.dLast = nz; float s = 0;
            for (int k = 0; k < 128; ++k) { float& e = t.dEnv[k]; if (e < 0.0005f) continue; float v;
                if (k == 35 || k == 36) { t.dPh[k] += (45.f + 130.f * e * e * e) / fs; v = std::sin(tp * t.dPh[k]) * e * 1.1f; e *= 0.99965f; }
                else if (k == 38 || k == 39 || k == 40) { t.dPh[k] += 190.f / fs; v = (nz * 0.8f + std::sin(tp * t.dPh[k]) * 0.4f) * e; e *= 0.9992f; }
                else if (k == 42 || k == 44 || k == 46) { v = hp * 0.6f * e; e *= (k == 46 ? 0.9998f : 0.9986f); }
                else { t.dPh[k] += (80.f + (float) (k % 12) * 15.f) / fs; v = std::sin(tp * t.dPh[k]) * e; e *= 0.9993f; }
                s += v * 0.35f * t.vel[k]; }
            d0[i] = d1[i] = s; }
    }
    void render(AudioBuffer<float>& buf, int n, double sr, const AudioBuffer<float>& in) {
        buf.clear();
        if (float* dst = p.recPtr.load()) {                      
            p.recBusy = true;
            if (float* nx = p.recNext.load()) {                                   
                const int from = p.recNextFrom, pos = p.recPos; if (pos > from) std::memcpy(nx + from, dst + from, (size_t) (pos - from) * sizeof(float));
                if (p.recPtr.compare_exchange_strong(dst, nx)) p.recCap = p.recNextCap.load();
                p.recNext = nullptr; }
            if (p.recPtr.load() == dst && p.playing && p.beat >= p.recFrom && in.getNumChannels() > p.recCh.load())
                p.recPos = core::recordBlock(dst, p.recCap.load(), p.recPos.load(), in.getReadPointer(p.recCh.load()), n);
            p.recBusy = false; }
        ScopedTryLock sl(p.lock);
        if (! sl.isLocked() || n > 4096 || buf.getNumChannels() < 2) {          
            ++p.skipCount;
            if (p.playing) { double nb = p.beat + p.bpm / 60.0 / sr * n; if (p.cycle && nb >= p.loopEnd) nb = p.loopStart + (nb - p.loopEnd); p.beat = nb; }
            was = p.playing; return; }
        bool anySolo = false; for (auto& t : p.tracks) anySolo |= t->solo;
        const bool play = p.playing; double b = p.beat; const double step = p.bpm / 60.0 / sr, end = b + step * n, mid = b + step * n * 0.5;
        for (int k = 0; k < 128; ++k) inc[k] = (float) (440.0 * std::pow(2.0, (k - 69) / 12.0) / sr);
        sendBuf.clear(0, n); float* sL = sendBuf.getWritePointer(0); float* sR = sendBuf.getWritePointer(1); bool anySend = false;
        for (auto& tp : p.tracks) {
            Track& t = *tp; bool on[128]; MidiBuffer midi, none; t.coll.removeNextBlockOfMessages(midi, n);
            for (int k = 0; k < 128; ++k) { on[k] = t.live[k]; if (on[k]) t.vel[k] = 1.f; }
            if (was && ! play) midi.addEvent(MidiMessage::allNotesOff(1), 0);
            if (play) for (auto& c : t.clips) for (auto& nt : c.notes) {
                if (c.mute) continue;
                if (nt.start >= c.len) continue; const double s = c.start + nt.start, e = jmin(s + nt.len, c.start + c.len);
                if (mid >= s && mid < e) { on[nt.pitch] = true; t.vel[nt.pitch] = (float) nt.vel / 100.f; }
                if (! t.plugin) continue;
                if (s >= b && s < end) midi.addEvent(MidiMessage::noteOn(1, nt.pitch, (uint8) jlimit(1, 127, nt.vel)), jlimit(0, n - 1, (int) ((s - b) / step)));
                if (e >= b && e < end) midi.addEvent(MidiMessage::noteOff(1, nt.pitch), jlimit(0, n - 1, (int) ((e - b) / step)));
            }
            tmp.clear(); AudioBuffer<float> view(tmp.getArrayOfWritePointers(), 2, n);
            float inPeak = 0;
            if (t.audio) {
                const bool hasIn = t.armed && in.getNumChannels() > t.inCh;
                if (hasIn) inPeak = core::peakOf(in.getReadPointer(t.inCh), n);                 
                if (hasIn && t.monitor) for (int ch = 0; ch < 2; ++ch) tmp.copyFrom(ch, 0, in, t.inCh, 0, n);
                if (play) for (auto& c : t.clips) if (c.wav && ! c.rec && ! c.mute && ! c.wav->empty()) {
                    if (end < c.start || b > c.start + c.len) continue;
                    const int len = (int) c.wav->size(); const float* d = c.wav->data();
                    const bool st = c.wavR && (int) c.wavR->size() == len; const float* dR = st ? c.wavR->data() : d;
                    const double sps = c.wavSr * 60.0 / p.bpm.load();
                    for (int i = 0; i < n; ++i) {
                        const double pb = b + i * step - c.start; if (pb < 0.0 || pb >= c.len) continue;
                        const double rp = (pb + c.offset) * sps; const float smpL = core::readInterp(d, len, rp), smpR = st ? core::readInterp(dR, len, rp) : smpL;
                        if (smpL != 0.f || smpR != 0.f) { float g = 1.f;
                            if (c.fadeIn > 0.0 && pb < c.fadeIn) g *= (float) jmax(0.0, pb / c.fadeIn);
                            if (c.fadeOut > 0.0 && pb > c.len - c.fadeOut) g *= (float) jmax(0.0, (c.len - pb) / c.fadeOut);
                            tmp.addSample(0, i, smpL * g); tmp.addSample(1, i, smpR * g); } } }
            }
            else if (t.plugin) t.plugin->processBlock(view, midi);
            else if (t.wave == 10) drums(t, on, n, sr);
            else synth(t, on, n, sr);
            eqBlock(t, view.getWritePointer(0), view.getWritePointer(1), n, sr);
            for (int s = 0; s < 4; ++s) { if (t.fx[s]) t.fx[s]->processBlock(view, none); else if (t.hasAmp(s)) t.ampSim->process(view.getWritePointer(0), view.getWritePointer(1), n); }          
            const bool audible = ! t.mute && (! anySolo || t.solo);
            const float V = audible ? t.vol : 0.f, L = V * (1.f - jmax(0.f, t.pan)), R = V * (1.f + jmin(0.f, t.pan)); float peak = 0;
            for (int i = 0; i < n; ++i) { const float l = view.getSample(0, i), r = view.getSample(1, i);
                buf.addSample(0, i, l * L); buf.addSample(1, i, r * R); t.sendS += ((audible ? t.send : 0.f) - t.sendS) * 0.004f; if (t.sendS > 0.0001f) { sL[i] += l * L * t.sendS; sR[i] += r * R * t.sendS; } peak = jmax(peak, std::abs(l), std::abs(r)); }
            t.level = jmax(peak * t.vol, inPeak, t.level * 0.92f); anySend |= (t.sendS > 0.0005f || (t.send > 0.0005f && audible));
        }
        {   
            if (anySend) revTail = (int) (sr * 4.0);
            if (revTail > 0) { if (std::abs(revSr - sr) > 1.0) { rev.setSampleRate(sr); rev.reset(); revSr = sr; }
                Reverb::Parameters rp; rp.roomSize = p.revRoom; rp.damping = p.revDamp; rp.wetLevel = p.revReturn; rp.dryLevel = 0.f; rp.width = 1.f; rp.freezeMode = 0.f; rev.setParameters(rp);
                rev.processStereo(sL, sR, n);
                { const float fadeLen = (float) (sr * 0.75);   // fade the last part of the tail instead of cutting it
                  if ((float) revTail < fadeLen) { const float g0 = jmax(0.f, (float) revTail / fadeLen), g1 = jmax(0.f, (float) (revTail - n) / fadeLen); sendBuf.applyGainRamp(0, 0, n, g0, g1); sendBuf.applyGainRamp(1, 0, n, g0, g1); } }
                buf.addFrom(0, 0, sendBuf, 0, 0, n); buf.addFrom(1, 0, sendBuf, 1, 0, n); revTail -= n;
                if (revTail <= 0) { revTail = 0; rev.reset(); } }   // flush old tail so it cannot come back as a burst
            AudioBuffer<float> mv(buf.getArrayOfWritePointers(), 2, n); MidiBuffer noMidi; Track& m = p.masterTrack;
            eqBlock(m, mv.getWritePointer(0), mv.getWritePointer(1), n, sr);
            for (auto& f : m.fx) if (f) f->processBlock(mv, noMidi);
            const float mg = m.mute ? 0.f : p.master.load(); float gl, gr; core::panGains(m.pan, gl, gr); gl *= mg; gr *= mg;
            auto* m0 = buf.getWritePointer(0); auto* m1 = buf.getWritePointer(1); for (int i = 0; i < n; ++i) { m0[i] *= gl; m1[i] *= gr; }
        }
        if (play && (p.counting || (p.metro && (! p.clickRecOnly || p.recClip >= 0)))) {
            const int bpb = jmax(1, p.beatsPerBar.load()), sub = jmax(1, p.clickSub.load()), type = p.clickType.load(); const bool acc = p.clickAccent.load();
            const double hi = p.clickHi.load(), lo = p.clickLo.load(), vol = p.clickVol.load(), tp = MathConstants<double>::twoPi;
            for (int i = 0; i < n; ++i) {
                const double bi = b + i * step, sb = bi * sub, sbPrev = (bi - step) * sub;
                if (std::floor(sb) != std::floor(sbPrev)) {                                         
                    const long long k = (long long) std::floor(sb), bk = (long long) std::floor(bi);
                    const bool onBeat = core::wrapIndex(k, sub) == 0, down = onBeat && core::isDownbeat(bk, bpb);
                    cenv = 1; cph = 0; cph2 = 0; cf = (acc && down) ? hi : lo; cgain = ! onBeat ? 0.45f : (acc && down) ? 1.f : (acc ? 0.75f : 1.f); }
                float c = 0;
                if (cenv > 0.0002) {
                    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; const float nz = (float) (rng & 0xffff) / 32768.f - 1.f, hp = nz - cLast; cLast = nz;
                    const float s1 = (float) std::sin(cph), s2 = (float) std::sin(cph2), e = (float) cenv;
                    switch (type) {
                        case 1:  c = (s1 * 0.85f + nz * 0.15f) * e; cenv *= 0.9955; break;                                        
                        case 2:  c = hp * 0.7f * e; cenv *= 0.9965; break;                                                        
                        case 3:  c = ((s1 > 0.f ? 0.5f : -0.5f) + (s2 > 0.f ? 0.5f : -0.5f)) * 0.6f * e; cenv *= 0.9993; break;   
                        default: c = s1 * e; cenv *= 0.9985; break; }                                                             
                    cph += tp * cf / sr; cph2 += tp * cf * 1.4835 / sr; if (cph > tp) cph -= tp; if (cph2 > tp) cph2 -= tp; }
                c *= (float) vol * cgain; buf.addSample(0, i, c); buf.addSample(1, i, c); }
        }
        {   
            const float pl = buf.getMagnitude(0, 0, n), pr = buf.getMagnitude(1, 0, n);
            p.masterLevelL = jmax(pl, p.masterLevelL.load() * 0.92f); p.masterLevelR = jmax(pr, p.masterLevelR.load() * 0.92f); if (jmax(pl, pr) > 1.f) p.masterClip = true; }
        const bool soft = p.masterLimit.load();
        for (int ch = 0; ch < buf.getNumChannels(); ++ch) { auto* d = buf.getWritePointer(ch); for (int i = 0; i < n; ++i) d[i] = soft ? jlimit(-1.f, 1.f, core::softLimit(d[i])) : jlimit(-1.f, 1.f, d[i]); }
        p.masterLevel = jmax(buf.getMagnitude(0, n), p.masterLevel.load() * 0.92f);
        if (play) { b += step * n; if (p.cycle && b >= p.loopEnd) b = p.loopStart + (b - p.loopEnd); p.beat = b; }
        was = play;
    }
};
struct DawLnF : LookAndFeel_V4 {
    void drawButtonBackground(Graphics& g, Button& b, const Colour& bg, bool over, bool down) override {      
        Colour c = bg; if (down) c = c.darker(0.25f); else if (over) c = c.brighter(0.12f);
        const auto br = b.getLocalBounds().toFloat().reduced(0.5f); g.setGradientFill(ColourGradient(c.brighter(0.07f), 0.f, br.getY(), c.darker(0.07f), 0.f, br.getBottom(), false)); const float rr = jmin(8.f, br.getHeight() * 0.3f); g.fillRoundedRectangle(br, rr);
        g.setColour(Colour(0x1affffff)); g.drawRoundedRectangle(br, rr, 1.f);
    }
    int getSliderThumbRadius(Slider& s) override { return s.isVertical() ? 13 : LookAndFeel_V4::getSliderThumbRadius(s); }
    Font getTextButtonFont(TextButton&, int h) override { return Font(FontOptions(jmin(13.f, (float) h * 0.55f), Font::bold)); }
    void drawLinearSlider(Graphics& g, int x, int y, int w, int h, float pos, float mn, float mx, Slider::SliderStyle st, Slider& s) override {
        if (st == Slider::LinearHorizontal && s.getProperties()["logic"]) {                       
            const float cy = (float) y + (float) h * 0.5f, x0 = (float) x + 8.f, x1 = (float) x + (float) w - 8.f;
            g.setColour(Colour(0xff17181a)); g.fillRoundedRectangle(x0 - 4.f, cy - 4.f, x1 - x0 + 8.f, 8.f, 4.f);
            g.setColour(Colour(0xff6c6d73)); g.fillRoundedRectangle(x0 - 4.f, cy - 4.f, jmax(8.f, pos - x0 + 4.f), 8.f, 4.f);
            g.setColour(Colour(0x55000000)); g.fillEllipse(pos - 8.f, cy - 7.f, 16.f, 16.f);
            g.setColour(Colour(0xffe3e3e7)); g.fillEllipse(pos - 8.f, cy - 8.f, 16.f, 16.f); return; }
        if (st == Slider::LinearBar || st == Slider::LinearHorizontal) {                              
            const float al = s.isEnabled() ? 1.f : 0.45f; const Colour accent = acc(s);
            if (st == Slider::LinearBar) { const auto r = Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(0.f, 1.f); g.setColour(Colour(0xff17181b).withMultipliedAlpha(al)); g.fillRoundedRectangle(r, 6.f);
                if (pos - r.getX() > 1.f) { g.setColour(accent.withAlpha(0.5f * al)); g.fillRoundedRectangle(r.withWidth(jlimit(8.f, r.getWidth(), pos - r.getX())), 6.f); }
                g.setColour(Colour(0xff34353a)); g.drawRoundedRectangle(r.reduced(0.5f), 6.f, 1.f); return; }
            const float cy = (float) y + (float) h * 0.5f, x0 = (float) x, x1 = (float) x + (float) w; const bool hot = s.isMouseOverOrDragging();
            g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle(x0, cy - 3.f, x1 - x0, 6.f, 3.f); g.setColour(Colour(0xff2e2f34)); g.drawRoundedRectangle(x0 + 0.5f, cy - 2.5f, x1 - x0 - 1.f, 5.f, 2.5f, 1.f);
            g.setColour(accent.withMultipliedAlpha(al)); g.fillRoundedRectangle(x0, cy - 3.f, jmax(6.f, pos - x0), 6.f, 3.f);
            const float rad = hot ? 9.f : 8.f; if (hot) { g.setColour(accent.withAlpha(0.25f)); g.fillEllipse(pos - 13.f, cy - 13.f, 26.f, 26.f); }
            g.setColour(Colour(0x66000000)); g.fillEllipse(pos - rad, cy - rad + 1.5f, rad * 2.f, rad * 2.f); g.setColour(Colour(0xfff2f2f5).withMultipliedAlpha(al)); g.fillEllipse(pos - rad, cy - rad, rad * 2.f, rad * 2.f);
            g.setColour(accent.withMultipliedAlpha(al)); g.fillEllipse(pos - 3.f, cy - 3.f, 6.f, 6.f); return; }
        if (st != Slider::LinearVertical) { LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, mn, mx, st, s); return; }
        const float cx = (float) x + (float) w * 0.5f; g.setColour(Colour(0xff0f1012)); g.fillRoundedRectangle(cx - 2.f, (float) y, 4.f, (float) h, 2.f);
        Rectangle<float> th(0.f, 0.f, (float) w - 2.f, 26.f); th.setCentre(cx, pos);                                       
        g.setColour(Colour(0x66000000)); g.fillRoundedRectangle(th.translated(0.f, 1.5f), 4.f);
        g.setGradientFill(ColourGradient(Colour(0xffe6e7eb), 0.f, th.getY(), Colour(0xff9b9ca2), 0.f, th.getBottom(), false)); g.fillRoundedRectangle(th, 4.f);
        g.setColour(Colour(0xff222326)); g.fillRect(th.getX() + 3.f, th.getCentreY() - 1.f, th.getWidth() - 6.f, 2.f);
        g.setColour(Colour(0x55ffffff)); g.fillRect(th.getX() + 3.f, th.getCentreY() - 6.f, th.getWidth() - 6.f, 1.f); g.fillRect(th.getX() + 3.f, th.getCentreY() + 5.f, th.getWidth() - 6.f, 1.f);
    }
    void drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& sl) override {      
        auto b = Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(2.f); const float ang = a0 + pos * (a1 - a0), cx = b.getCentreX(), cy = b.getCentreY();
        const float ring = b.getWidth() * 0.5f - 1.f, rad = ring - 5.f; const bool bipolar = sl.getMinimum() < 0.0 && sl.getMaximum() > 0.0; const float from = bipolar ? (a0 + a1) * 0.5f : a0;
        Path track; track.addCentredArc(cx, cy, ring, ring, 0.f, a0, a1, true); g.setColour(Colour(0xff17181a)); g.strokePath(track, PathStrokeType(2.5f));
        if (std::abs(ang - from) > 0.01f) { Path v; v.addCentredArc(cx, cy, ring, ring, 0.f, jmin(from, ang), jmax(from, ang), true); g.setColour(findColour(Slider::trackColourId)); g.strokePath(v, PathStrokeType(2.5f)); }
        g.setGradientFill(ColourGradient(Colour(0xff5a5b62), cx, cy - rad, Colour(0xff34353a), cx, cy + rad, false)); g.fillEllipse(cx - rad, cy - rad, rad * 2.f, rad * 2.f);
        g.setColour(Colour(0xff111214)); g.drawEllipse(cx - rad, cy - rad, rad * 2.f, rad * 2.f, 1.f);
        Path pth; pth.addRoundedRectangle(-1.2f, -rad + 1.5f, 2.4f, rad * 0.55f, 1.f); g.setColour(Colour(0xfff0f0f4)); g.fillPath(pth, AffineTransform::rotation(ang).translated(cx, cy));
    }
    Colour accentCol { 0xff5b8def };
    void applyTheme(Colour acc) {                                                   
        accentCol = acc; const Colour field(0xff17181b), txt(0xfff0f0f4), dim(0xffa0a1a9);
        setColour(TextEditor::backgroundColourId, field); setColour(TextEditor::textColourId, txt); setColour(TextEditor::outlineColourId, Colours::transparentBlack); setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
        setColour(TextEditor::highlightColourId, acc.withAlpha(0.45f)); setColour(TextEditor::highlightedTextColourId, Colours::white); setColour(CaretComponent::caretColourId, Colours::white);
        setColour(ComboBox::backgroundColourId, field); setColour(ComboBox::outlineColourId, Colour(0xff3a3b41)); setColour(ComboBox::textColourId, txt); setColour(ComboBox::arrowColourId, dim); setColour(ComboBox::focusedOutlineColourId, acc);
        setColour(Slider::textBoxTextColourId, txt); setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack); setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack); setColour(Slider::textBoxHighlightColourId, acc.withAlpha(0.45f));
        setColour(Slider::trackColourId, acc); setColour(Slider::backgroundColourId, field); setColour(Slider::thumbColourId, Colours::white);
        setColour(PopupMenu::backgroundColourId, Colour(0xff2a2b30)); setColour(PopupMenu::textColourId, Colour(0xffe4e4e8)); setColour(PopupMenu::headerTextColourId, dim); setColour(PopupMenu::highlightedBackgroundColourId, acc); setColour(PopupMenu::highlightedTextColourId, Colours::white);
        setColour(ToggleButton::textColourId, txt); setColour(ToggleButton::tickColourId, acc); setColour(ScrollBar::thumbColourId, Colour(0xff5e5f66));
        setColour(AlertWindow::backgroundColourId, Colour(0xff2b2d31)); setColour(AlertWindow::outlineColourId, Colour(0xff0d0e10)); setColour(AlertWindow::textColourId, txt);
        setColour(TextButton::buttonOnColourId, acc); }
    Colour acc(const Component& c) const { for (const Component* q = &c; q != nullptr; q = q->getParentComponent()) if (q->getProperties().contains("mono")) return Colour(0xffe8e8ec); return accentCol; }      
    struct Dot : Button {
        int type; Dot(const String& n, int t) : Button(n), type(t) { setMouseCursor(MouseCursor::PointingHandCursor); }
        void paintButton(Graphics& g, bool over, bool down) override {
            const auto r = getLocalBounds().toFloat().withSizeKeepingCentre(14.f, 14.f); g.setColour(down ? Colour(0xff5d5e65) : over ? Colour(0xffe2e3e8) : Colour(0xff8c8d94)); g.fillEllipse(r);
            if (over) { g.setColour(Colour(0xff1d1e22)); const auto c = r.reduced(4.5f); if (type == 4) { g.drawLine(c.getX(), c.getY(), c.getRight(), c.getBottom(), 1.5f); g.drawLine(c.getRight(), c.getY(), c.getX(), c.getBottom(), 1.5f); }
                else if (type == 1) g.drawLine(c.getX(), c.getCentreY(), c.getRight(), c.getCentreY(), 1.5f); else { g.drawLine(c.getX(), c.getCentreY(), c.getRight(), c.getCentreY(), 1.5f); g.drawLine(c.getCentreX(), c.getY(), c.getCentreX(), c.getBottom(), 1.5f); } } } };
    Button* createDocumentWindowButton(int buttonType) override { return new Dot(buttonType == 4 ? "close" : buttonType == 1 ? "minimise" : "maximise", buttonType); }
    void positionDocumentWindowButtons(DocumentWindow&, int tx, int ty, int, int th, Button* mn, Button* mx, Button* cl, bool) override {
        int x = tx + 14; const int y = ty + (th - 18) / 2; for (auto* b : { cl, mn, mx }) if (b != nullptr) { b->setBounds(x, y, 18, 18); x += 24; } }
    void drawDocumentWindowTitleBar(DocumentWindow& win, Graphics& g, int w, int h, int, int, const Image*, bool) override {
        g.setGradientFill(ColourGradient(Colour(0xff36373c), 0.f, 0.f, Colour(0xff2c2d31), 0.f, (float) h, false)); g.fillRect(0, 0, w, h); g.setColour(Colour(0xff111214)); g.fillRect(0, h - 1, w, 1);
        g.setColour(Colour(0xffc9cad2)); g.setFont(Font(FontOptions(13.5f, Font::bold))); g.drawText(win.getName(), 90, 0, jmax(10, w - 180), h, Justification::centred, true); }
    void drawCallOutBoxBackground(CallOutBox& box, Graphics& g, const Path& path, Image& cached) override {
        if (cached.isNull()) { cached = Image(Image::ARGB, box.getWidth(), box.getHeight(), true); Graphics g2(cached); DropShadow(Colours::black.withAlpha(0.6f), 16, Point<int>()).drawForPath(g2, path); }
        g.setColour(Colours::black); g.drawImageAt(cached, 0, 0); g.setColour(Colour(0xff25262b)); g.fillPath(path); g.setColour(Colour(0xff40414a)); g.strokePath(path, PathStrokeType(1.f)); }
    void fillTextEditorBackground(Graphics& g, int w, int h, TextEditor& t) override { g.setColour(t.findColour(TextEditor::backgroundColourId)); g.fillRoundedRectangle(0.f, 0.f, (float) w, (float) h, 7.f); }
    void drawTextEditorOutline(Graphics& g, int w, int h, TextEditor& t) override {
        const Colour accent = acc(t); const bool focus = t.hasKeyboardFocus(true) && ! t.isReadOnly(); const auto r = Rectangle<float>(0.f, 0.f, (float) w, (float) h).reduced(0.5f);
        if (focus) { g.setColour(accent.withAlpha(0.2f)); g.drawRoundedRectangle(r.reduced(1.5f), 6.f, 2.f); }
        g.setColour(focus ? accent : Colour(0xff3a3b41)); g.drawRoundedRectangle(r, 7.f, focus ? 1.6f : 1.f); }
    void drawComboBox(Graphics& g, int w, int h, bool down, int, int, int, int, ComboBox& box) override {
        const Colour accent = acc(box); const auto r = Rectangle<float>(0.f, 0.f, (float) w, (float) h).reduced(0.5f); const bool hot = box.hasKeyboardFocus(false) || down || box.isMouseOver();
        g.setColour(box.findColour(ComboBox::backgroundColourId)); g.fillRoundedRectangle(r, 7.f); g.setColour(down || box.hasKeyboardFocus(false) ? accent : hot ? Colour(0xff55565e) : box.findColour(ComboBox::outlineColourId)); g.drawRoundedRectangle(r, 7.f, down ? 1.6f : 1.f);
        const float cx = (float) w - 15.f, cy = (float) h * 0.5f; Path c; c.startNewSubPath(cx - 4.f, cy - 2.f); c.lineTo(cx, cy + 2.f); c.lineTo(cx + 4.f, cy - 2.f);
        g.setColour(box.findColour(ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 1.f : 0.4f)); g.strokePath(c, PathStrokeType(1.6f, PathStrokeType::curved, PathStrokeType::rounded)); }
    void positionComboBoxText(ComboBox& box, Label& label) override { label.setBounds(10, 1, box.getWidth() - 30, box.getHeight() - 2); label.setFont(getComboBoxFont(box)); }
    Font getComboBoxFont(ComboBox& b) override { return Font(FontOptions(jmin(14.f, (float) b.getHeight() * 0.62f))); }
    void drawLabel(Graphics& g, Label& l) override {
        if (auto* s = dynamic_cast<Slider*>(l.getParentComponent())) {
            const Colour accent = acc(l); const auto r = l.getLocalBounds().toFloat().reduced(0.5f); const bool bar = s->isBar();
            if (! bar) { g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle(r, 6.f); g.setColour(l.isBeingEdited() ? accent : Colour(0xff34353a)); g.drawRoundedRectangle(r, 6.f, l.isBeingEdited() ? 1.5f : 1.f); }
            if (! l.isBeingEdited()) { g.setColour(l.findColour(Label::textColourId).withMultipliedAlpha(l.isEnabled() ? 1.f : 0.5f)); g.setFont(l.getFont()); g.drawText(l.getText(), l.getLocalBounds().reduced(4, 0), l.getJustificationType(), true); }
            return; }
        LookAndFeel_V4::drawLabel(g, l); }
    void drawToggleButton(Graphics& g, ToggleButton& b, bool over, bool) override {
        const Colour accent = acc(b); const bool on = b.getToggleState(); const float H = 20.f, W = 36.f; const auto r = Rectangle<float>(2.f, ((float) b.getHeight() - H) * 0.5f, W, H); const float al = b.isEnabled() ? 1.f : 0.45f;
        g.setColour((on ? accent : Colour(0xff3a3b41)).withMultipliedAlpha(al)); g.fillRoundedRectangle(r, H * 0.5f); g.setColour(over ? Colour(0x44ffffff) : Colour(0x22ffffff)); g.drawRoundedRectangle(r.reduced(0.5f), H * 0.5f, 1.f);
        const float kx = on ? r.getRight() - H * 0.5f : r.getX() + H * 0.5f, ky = r.getCentreY(); g.setColour(Colour(0x55000000)); g.fillEllipse(kx - 8.f, ky - 6.5f, 16.f, 16.f); g.setColour(Colours::white.withMultipliedAlpha(al)); g.fillEllipse(kx - 8.f, ky - 8.f, 16.f, 16.f);
        if (b.getButtonText().isNotEmpty()) { g.setColour(b.findColour(ToggleButton::textColourId).withMultipliedAlpha(al)); g.setFont(Font(FontOptions(14.f))); g.drawText(b.getButtonText(), (int) r.getRight() + 10, 0, b.getWidth() - (int) r.getRight() - 12, b.getHeight(), Justification::centredLeft, true); } }
    void drawScrollbar(Graphics& g, ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override {
        if (thumbSize <= 0) return;
        const auto track = vertical ? Rectangle<float>((float) x + 3.f, (float) y, (float) w - 6.f, (float) h) : Rectangle<float>((float) x, (float) y + 3.f, (float) w, (float) h - 6.f);
        const auto th = vertical ? Rectangle<float>(track.getX(), (float) y + (float) thumbStart, track.getWidth(), (float) thumbSize) : Rectangle<float>((float) x + (float) thumbStart, track.getY(), (float) thumbSize, track.getHeight());
        g.setColour(Colour(0x0affffff)); g.fillRoundedRectangle(track, 4.f); g.setColour(down ? accentCol : over ? Colour(0xff8b8c94) : Colour(0xff5e5f66)); g.fillRoundedRectangle(th, jmin(th.getWidth(), th.getHeight()) * 0.5f); }
    void drawTabButton(TabBarButton& b, Graphics& g, bool over, bool) override {
        const auto r = b.getActiveArea().toFloat(); const bool front = b.isFrontTab();
        if (over && ! front) { g.setColour(Colour(0x12ffffff)); g.fillRoundedRectangle(r.reduced(3.f, 4.f), 6.f); }
        g.setColour(front ? Colours::white : Colour(0xffa0a1a9)); g.setFont(Font(FontOptions(14.f, front ? Font::bold : Font::plain))); g.drawText(b.getButtonText(), b.getTextArea(), Justification::centred, true);
        if (front) { g.setColour(accentCol); g.fillRoundedRectangle(r.getX() + 8.f, r.getBottom() - 3.f, r.getWidth() - 16.f, 3.f, 1.5f); } }
    void drawTabAreaBehindFrontButton(TabbedButtonBar&, Graphics& g, int w, int h) override { g.setColour(Colour(0xff3a3b41)); g.fillRect(0, h - 1, w, 1); }
    void drawPopupMenuBackground(Graphics& g, int w, int h) override { g.fillAll(Colour(0xff2a2b30)); g.setColour(Colour(0xff0d0e10)); g.drawRect(0, 0, w, h, 1); }
    void drawPopupMenuItem(Graphics& g, const Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu, const String& text, const String& shortcut, const Drawable* icon, const Colour* textColour) override {
        if (isSeparator) { g.setColour(Colour(0xff3d3e44)); g.fillRect(area.reduced(10, 0).withHeight(1).withY(area.getCentreY())); return; }
        const bool hot = isHighlighted && isActive; if (hot) { g.setColour(accentCol); g.fillRoundedRectangle(area.toFloat().reduced(4.f, 1.f), 6.f); }
        Colour c = textColour != nullptr ? *textColour : Colour(0xffe4e4e8); if (hot) c = Colours::white; if (! isActive) c = c.withAlpha(0.4f);
        g.setColour(c); const int tx = area.getX() + 30;
        if (isTicked) { Path t; const float cx = (float) area.getX() + 17.f, cy = (float) area.getCentreY(); t.startNewSubPath(cx - 4.f, cy); t.lineTo(cx - 1.f, cy + 3.f); t.lineTo(cx + 4.5f, cy - 3.5f); g.strokePath(t, PathStrokeType(1.8f, PathStrokeType::curved, PathStrokeType::rounded)); }
        if (icon != nullptr) icon->drawWithin(g, Rectangle<float>((float) area.getX() + 8.f, (float) area.getY() + 4.f, 16.f, (float) area.getHeight() - 8.f), RectanglePlacement::centred, 1.f);
        g.setFont(Font(FontOptions(14.f))); g.drawText(text, tx, area.getY(), area.getWidth() - 30 - (shortcut.isNotEmpty() ? 90 : 20), area.getHeight(), Justification::centredLeft, true);
        if (shortcut.isNotEmpty()) { g.setColour(c.withAlpha(hot ? 0.8f : 0.5f)); g.setFont(Font(FontOptions(12.5f))); g.drawText(shortcut, area.getRight() - 100, area.getY(), 88, area.getHeight(), Justification::centredRight, false); }
        if (hasSubMenu) { Path a; const float ax = (float) area.getRight() - 14.f, ay = (float) area.getCentreY(); a.startNewSubPath(ax - 2.f, ay - 4.f); a.lineTo(ax + 2.f, ay); a.lineTo(ax - 2.f, ay + 4.f); g.setColour(c); g.strokePath(a, PathStrokeType(1.6f, PathStrokeType::curved, PathStrokeType::rounded)); } }
    void drawPopupMenuItemWithOptions(Graphics& g, const Rectangle<int>& area, bool isHighlighted, const PopupMenu::Item& item, const PopupMenu::Options&) override {
        drawPopupMenuItem(g, area, item.isSeparator, item.isEnabled, isHighlighted, item.isTicked, item.subMenu != nullptr, item.text, item.shortcutKeyDescription, item.image.get(), item.colour != Colour() ? &item.colour : nullptr); }
};
static constexpr int kTrackIcons = 9;   // 0 = automatic (by track type), 1..8 = glyphs
inline void drawTrackIcon(Graphics& g, Rectangle<float> ic, Colour col, int kind, bool audio) {
    if (kind <= 0 || kind >= kTrackIcons) kind = audio ? 1 : 2;
    const Colour dark = col.darker(0.55f), white = Colours::white.withAlpha(0.92f);
    g.setColour(dark); g.fillRoundedRectangle(ic, 4.f); g.setColour(white);
    const float x = ic.getX(), y = ic.getY(), cx = ic.getCentreX(), cy = ic.getCentreY(), u = ic.getWidth() / 20.f;
    switch (kind) {
        case 1: { static const float hs[7] = { 3, 7, 11, 5, 9, 4, 2 }; for (int k = 0; k < 7; ++k) g.fillRect(x + 3.f * u + (float) k * 2.1f * u, cy - hs[k] * 0.5f * u, 1.4f * u, hs[k] * u); break; }
        case 2: { for (int k = 0; k < 4; ++k) g.fillRect(x + 3.f * u + (float) k * 3.6f * u, y + 4.f * u, 2.6f * u, 12.f * u); g.setColour(dark); for (int k = 0; k < 3; ++k) g.fillRect(x + 5.2f * u + (float) k * 3.6f * u, y + 4.f * u, 2.f * u, 7.f * u); break; }
        case 3: { g.fillRoundedRectangle(x + 4.f * u, y + 9.f * u, 12.f * u, 8.f * u, 2.f * u); g.setColour(Colours::white); g.fillEllipse(x + 4.f * u, y + 7.f * u, 12.f * u, 5.f * u); g.setColour(white);
                  g.drawLine(x + 5.f * u, y + 3.f * u, x + 10.f * u, y + 8.f * u, 1.6f * u); g.drawLine(x + 15.f * u, y + 3.f * u, x + 10.f * u, y + 8.f * u, 1.6f * u); break; }
        case 4: { g.fillEllipse(x + 3.f * u, y + 10.f * u, 8.f * u, 8.f * u); g.fillEllipse(x + 5.f * u, y + 7.f * u, 6.f * u, 6.f * u); g.drawLine(x + 9.f * u, y + 11.f * u, x + 16.f * u, y + 4.f * u, 2.f * u); g.fillEllipse(x + 14.5f * u, y + 2.f * u, 3.5f * u, 3.5f * u);
                  g.setColour(dark); g.fillEllipse(x + 5.5f * u, y + 12.5f * u, 3.f * u, 3.f * u); break; }
        case 5: { g.fillRoundedRectangle(x + 7.f * u, y + 3.f * u, 6.f * u, 9.f * u, 3.f * u); Path a; a.addCentredArc(cx, y + 9.f * u, 5.5f * u, 5.5f * u, 0.f, MathConstants<float>::halfPi, MathConstants<float>::pi * 1.5f, true);
                  g.strokePath(a, PathStrokeType(1.5f * u)); g.fillRect(cx - 0.75f * u, y + 14.5f * u, 1.5f * u, 3.f * u); g.fillRect(cx - 3.f * u, y + 17.f * u, 6.f * u, 1.5f * u); break; }
        case 6: { g.fillEllipse(x + 4.f * u, y + 12.f * u, 6.f * u, 4.5f * u); g.fillRect(x + 9.f * u, y + 4.f * u, 1.6f * u, 10.f * u); Path f; f.startNewSubPath(x + 10.6f * u, y + 4.f * u); f.quadraticTo(x + 15.f * u, y + 6.f * u, x + 14.f * u, y + 10.f * u); g.strokePath(f, PathStrokeType(1.6f * u)); break; }
        case 7: { Path w; for (int i = 0; i <= 16; ++i) { const float px = x + 3.f * u + (float) i * 14.f * u / 16.f, py = cy - std::sin((float) i / 16.f * MathConstants<float>::twoPi) * 5.f * u; if (i == 0) w.startNewSubPath(px, py); else w.lineTo(px, py); }
                  g.strokePath(w, PathStrokeType(1.8f * u)); break; }
        default: { Path w; w.startNewSubPath(x + 3.f * u, y + 14.f * u); w.lineTo(x + 8.f * u, y + 6.f * u); w.lineTo(x + 8.f * u, y + 14.f * u); w.lineTo(x + 13.f * u, y + 6.f * u); w.lineTo(x + 13.f * u, y + 14.f * u); w.lineTo(x + 17.f * u, y + 9.f * u);
                   g.strokePath(w, PathStrokeType(1.6f * u)); break; }
    }
}
struct TrackLook : Component, ChangeListener {
    Project& p; Track& t; ColourSelector cs { ColourSelector::showColourspace, 4, 4 };
    static constexpr int W = 296, H = 290;
    static const Colour* swatch(int i) { static const Colour c[16] = { Colour(0xffff453a), Colour(0xffff7a3d), Colour(0xffff9f0a), Colour(0xffffd60a), Colour(0xffc7e03a), Colour(0xff30d158), Colour(0xff2ec4a6), Colour(0xff64d2ff),
        Colour(0xff4c8dff), Colour(0xff5e5ce6), Colour(0xffbf5af2), Colour(0xffff5fa2), Colour(0xffa2845e), Colour(0xff8e8e93), Colour(0xffc8c8cc), Colour(0xff5a6b7a) }; return &c[i]; }
    Rectangle<int> swRect(int i) const { return Rectangle<int>(12 + (i % 8) * 34, 12 + (i / 8) * 28, 30, 22); }
    Rectangle<int> icRect(int i) const { return Rectangle<int>(12 + i * 30, 92, 26, 26); }
    TrackLook(Project& pr, Track& tr) : p(pr), t(tr) { setSize(W, H); cs.setCurrentColour(t.colour, dontSendNotification); cs.addChangeListener(this); addAndMakeVisible(cs); }
    ~TrackLook() override { cs.removeChangeListener(this); }
    void resized() override { cs.setBounds(8, 126, W - 16, H - 134); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2a2b30)); g.setFont(Font(FontOptions(11.f))); g.setColour(Colour(0xff9a9ba2)); g.drawText("Icon", 12, 72, 100, 14, Justification::centredLeft, false);
        for (int i = 0; i < 16; ++i) { const auto r = swRect(i).toFloat(); g.setColour(*swatch(i)); g.fillRoundedRectangle(r, 4.f);
            if (t.colour.withAlpha(1.f) == swatch(i)->withAlpha(1.f)) { g.setColour(Colours::white); g.drawRoundedRectangle(r.expanded(2.f), 5.f, 2.f); } }
        for (int i = 0; i < kTrackIcons; ++i) { auto r = icRect(i).toFloat(); drawTrackIcon(g, r, t.colour, i, t.audio);
            if (i == 0) { g.setColour(Colours::white.withAlpha(0.7f)); g.setFont(Font(FontOptions(8.f, Font::bold))); g.drawText("AUTO", r.removeFromBottom(8.f).toNearestInt(), Justification::centred, false); }
            if (t.icon == i) { g.setColour(Colours::white); g.drawRoundedRectangle(icRect(i).toFloat().expanded(2.f), 5.f, 2.f); } }
    }
    void mouseDown(const MouseEvent& e) override {
        for (int i = 0; i < 16; ++i) if (swRect(i).contains(e.getPosition())) { { ScopedLock sl(p.lock); t.colour = *swatch(i); } cs.setCurrentColour(t.colour, dontSendNotification); repaint(); return; }
        for (int i = 0; i < kTrackIcons; ++i) if (icRect(i).contains(e.getPosition())) { { ScopedLock sl(p.lock); t.icon = i; } repaint(); return; } }
    void changeListenerCallback(ChangeBroadcaster*) override { { ScopedLock sl(p.lock); t.colour = cs.getCurrentColour().withAlpha(1.f); } repaint(); }
};
struct Arrangement : Component, Timer {
    Project& p; std::function<void()> onSelect, onZoom; std::function<void(int)> onMenu, onRename, onCmd;
    static constexpr int head = 196, ruler = 28, laneH = 64; double ppb = 24;
    int dragT = -1, dragC = -1; bool dragPushed = false; double dragOff = 0;
    int dragMode = 0; double dStart = 0, dLen = 0, dOffs = 0; std::vector<Note> dNotes; bool lenChanged = false;   // dragMode: 1 move, 2 trim left, 3 trim right, 4 scrub playhead
    int loopDrag = 0; double loopGrab = 0;   // 0 none, 1 move, 2 left edge, 3 right edge
    explicit Arrangement(Project& pr) : p(pr) { setWantsKeyboardFocus(true); startTimerHz(30); }
    void timerCallback() override { repaint(); }
    static Rectangle<int> btn(int ti, int k) { return { 118 + k * 25, ruler + ti * laneH + (laneH - 20) / 2, 22, 20 }; }
    int hx() const { if (auto* v = findParentComponentOfClass<Viewport>()) return v->getViewPositionX(); return 0; }   // pinned header x
    int loopZone(Point<int> pt) const {
        if (! p.cycle || pt.y < 4 || pt.y >= ruler - 4 || pt.x < hx() + head) return 0;
        const int xa = head + (int) (p.loopStart * ppb), xb = head + (int) (p.loopEnd * ppb);
        if (std::abs(pt.x - xa) <= 6) return 2; if (std::abs(pt.x - xb) <= 6) return 3; return pt.x > xa && pt.x < xb ? 1 : 0; }
    int edgeZone(Point<int> pt) const {
        if (pt.y < ruler || pt.x < hx() + head) return 0; const int ti = (pt.y - ruler) / laneH; if (ti < 0 || ti >= (int) p.tracks.size()) return 0;
        const double b = beatAt(pt.x);
        for (auto& c : p.tracks[(size_t) ti]->clips) if (! c.rec && b >= c.start && b < c.start + c.len) { const double edge = jmin(7.0 / ppb, c.len * 0.3); if (b - c.start < edge) return 2; if (c.start + c.len - b < edge) return 3; return 0; }
        return 0; }
    void mouseMove(const MouseEvent& e) override { const int z = loopZone(e.getPosition()); const int ez = z == 0 ? edgeZone(e.getPosition()) : 0;
        setMouseCursor(z == 2 || z == 3 || ez != 0 ? MouseCursor::LeftRightResizeCursor : z == 1 ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor); }
    void mouseUp(const MouseEvent&) override { loopDrag = 0; dragT = -1; dragMode = 0; if (lenChanged) { lenChanged = false; if (onZoom) onZoom(); } }
    Rectangle<int> iconBox(int ti) const { return { hx() + 30, ruler + ti * laneH + (laneH - 20) / 2, 20, 20 }; }
    void openLook(int ti) { if (ti < 0 || ti >= (int) p.tracks.size()) return; CallOutBox::launchAsynchronously(std::make_unique<TrackLook>(p, *p.tracks[(size_t) ti]), localAreaToGlobal(iconBox(ti)), nullptr); }
    Rectangle<int> hbtn(int ti, int k) const { return btn(ti, k).translated(hx(), 0); }   
    void paint(Graphics& g) override {
        const int W = getWidth(), H = getHeight(); const int hxp = this->hx();
        g.fillAll(Colour(0xff1f2023)); g.setGradientFill(ColourGradient(Colour(0xff3a3b40), 0, 0, Colour(0xff2d2e33), 0, (float) ruler, false)); g.fillRect(0, 0, W, ruler); g.setColour(Colour(0xff121315)); g.fillRect(0, ruler - 1, W, 1);
        if (p.cycle) { g.setColour(Colour(0x77e8c53a)); const int lx = head + (int) (p.loopStart * ppb), lw = (int) ((p.loopEnd - p.loopStart) * ppb); g.fillRect(lx, 4, lw, ruler - 8); g.setColour(Colour(0xffe8c53a)); g.fillRect(lx, 4, 2, ruler - 8); g.fillRect(lx + lw - 2, 4, 2, ruler - 8); }
        const int bpb = jmax(1, p.beatsPerBar.load());
        for (int bar = jmax(0, (int) ((g.getClipBounds().getX() - head) / (bpb * ppb)) - 1); bar * bpb < p.length && head + bar * bpb * ppb < g.getClipBounds().getRight(); ++bar) {   
            const int x = head + (int) (bar * bpb * ppb), w = (int) (bpb * ppb);
            if (bar % 2 == 1) { g.setColour(Colour(0x0affffff)); g.fillRect(x, ruler, w, H - ruler); }
            g.setColour(Colour(0xff3a3b40)); g.drawVerticalLine(x, (float) ruler - 10.f, (float) H);
            for (int q = 1; q < bpb; ++q) g.drawVerticalLine(x + (int) (q * ppb), (float) ruler - 5.f, (float) ruler);
            g.setColour(Colour(0xffa4a4aa)); g.setFont(12.f); g.drawText(String(bar + 1), x + 5, 0, 40, ruler - 8, Justification::centredLeft);
        }
        const char* lt[] = { "M", "S", "R" }; const Colour cc[] = { Colour(0xff4c8dff), Colour(0xffffd60a), Colour(0xffff453a) };   
        for (size_t ti = 0; ti < p.tracks.size(); ++ti) { auto& t = *p.tracks[ti]; const int y = ruler + (int) ti * laneH; 
            g.setColour(Colour(0xff151618)); g.drawHorizontalLine(y + laneH - 1, 0.f, (float) W);
            for (size_t ci = 0; ci < t.clips.size(); ++ci) { auto& c = t.clips[ci];
                Rectangle<float> r((float) head + (float) (c.start * ppb), (float) y + 4.f, (float) (c.len * ppb), (float) laneH - 8.f);
                const bool sel = (int) ti == p.selTrack && (int) ci == p.selClip; const Colour base = c.rec ? Colour(0xffe5413b) : c.mute ? Colour(0xff6a6b70) : t.colour;
                g.setColour(base.withMultipliedBrightness(0.5f)); g.fillRoundedRectangle(r, 4.f);
                g.setColour(base.withMultipliedBrightness(sel ? 1.f : 0.8f)); g.fillRoundedRectangle(r.withHeight(15.f), 4.f);
                g.setColour(Colours::black.withAlpha(0.8f)); g.setFont(11.f); g.drawText(c.rec ? String("Recording...") : c.mute ? t.name + " (muted)" : t.name, r.withHeight(15.f).reduced(5.f, 0.f).toNearestInt(), Justification::centredLeft);
                if (c.wav && ! c.peaks.empty() && p.showWave) {                                                  
                    Graphics::ScopedSaveState ss(g); g.reduceClipRegion(r.toNearestInt());                              
                    const float mid = r.getY() + 15.f + (r.getHeight() - 15.f) * 0.5f, hh = (r.getHeight() - 24.f) * 0.5f * 0.92f;
                    const double bpp = (c.wavSr * 60.0 / p.bpm.load() / ppb) / core::kPeakBlock; g.setColour(Colours::white.withAlpha(0.85f));
                    const double pk0 = c.offset * (c.wavSr * 60.0 / p.bpm.load()) / core::kPeakBlock; const auto cb = g.getClipBounds();
                    const int xs = jmax(0, cb.getX() - (int) r.getX() - 1), xe = jmin((int) r.getWidth(), cb.getRight() - (int) r.getX() + 1);
                    for (int x = xs; x < xe; ++x) { const int i0 = (int) (pk0 + x * bpp), i1 = jmax(i0 + 1, (int) (pk0 + (x + 1) * bpp)); float m = 0;
                        for (int i = i0; i < jmin(i1, (int) c.peaks.size()); ++i) m = jmax(m, c.peaks[(size_t) i]);
                        m = jmin(m, 1.f); if (m > 0.f) g.drawVerticalLine((int) r.getX() + x, mid - m * hh, mid + m * hh); } }
                if (p.showNotes) for (auto& n : c.notes) { const float ny = r.getBottom() - 5.f - (float) (jlimit(Project::lo, Project::hi, n.pitch) - Project::lo) / (float) (Project::hi - Project::lo) * (r.getHeight() - 24.f);
                    g.setColour(Colours::white.withAlpha(0.9f)); g.fillRect(r.getX() + (float) (n.start * ppb), ny, jmax(2.f, (float) (n.len * ppb)), 2.f); }
                if (c.fadeIn > 0.0) { g.setColour(Colours::white.withAlpha(0.7f)); g.drawLine(r.getX(), r.getBottom() - 2.f, r.getX() + (float) (c.fadeIn * ppb), r.getY() + 16.f, 1.2f); }
                if (c.fadeOut > 0.0) { g.setColour(Colours::white.withAlpha(0.7f)); g.drawLine(r.getRight() - (float) (c.fadeOut * ppb), r.getY() + 16.f, r.getRight(), r.getBottom() - 2.f, 1.2f); }
                if (sel) { g.setColour(Colours::white); g.drawRoundedRectangle(r, 4.f, 1.5f); } }
        }
        for (auto& mk : p.markers) { const int mx = head + (int) (mk.first * ppb); g.setColour(Colour(0x55ff9f0a)); g.drawVerticalLine(mx, (float) ruler, (float) H);
            g.setColour(Colour(0xffff9f0a)); g.fillRect(mx, ruler - 12, jmin(70, 8 + mk.second.length() * 6), 11); g.setColour(Colours::black); g.setFont(9.f); g.drawText(mk.second, mx + 3, ruler - 12, 66, 11, Justification::centredLeft, false); }
        const int px = head + (int) (p.beat * ppb); Path tri; tri.addTriangle((float) px - 6.f, 0.f, (float) px + 6.f, 0.f, (float) px, 12.f);
        g.setColour(Colours::white); g.fillPath(tri); g.drawVerticalLine(px, 0.f, (float) H);
        // ---- pinned track headers (drawn last, offset by the viewport scroll so they stay fixed) ----
        g.setColour(Colour(0xff1f2023)); g.fillRect(hxp, ruler, head, H - ruler);
        g.setGradientFill(ColourGradient(Colour(0xff3a3b40), 0, 0, Colour(0xff2d2e33), 0, (float) ruler, false)); g.fillRect(hxp, 0, head, ruler); g.setColour(Colour(0xff121315)); g.fillRect(hxp, ruler - 1, head, 1);
        for (size_t ti = 0; ti < p.tracks.size(); ++ti) { auto& t = *p.tracks[ti]; const int y = ruler + (int) ti * laneH; const bool st[] = { t.mute, t.solo, t.armed };
            Graphics::ScopedSaveState hdrState(g); g.setOrigin(hxp, 0); g.reduceClipRegion(0, y, head, laneH);
            g.setColour((int) ti == p.selTrack ? Colour(0xff3a3c45) : Colour(0xff2b2c30)); g.fillRect(0, y, head, laneH);
            g.setColour(t.colour); g.fillRect(0, y, 6, laneH);
            g.setColour(Colour(0xff151618)); g.drawHorizontalLine(y + laneH - 1, 0.f, (float) head);
            g.setColour(Colour(0xff8c8c92)); g.setFont(12.f); g.drawText(String((int) ti + 1), 8, y, 20, laneH, Justification::centred);
            { drawTrackIcon(g, Rectangle<float>(30.f, (float) y + ((float) laneH - 20.f) * 0.5f, 20.f, 20.f), t.colour, t.icon, t.audio); }
            g.setColour(Colours::white); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText(t.name, 54, y + 8, 62, 22, Justification::centredLeft); g.setColour(Colour(0xff9a9aa2)); g.setFont(11.f);
            g.drawText(t.audio ? String("Audio") : t.plugin ? t.plugin->getName() : String(Project::presets[t.preset].name), 54, y + 32, 62, 16, Justification::centredLeft);
            for (int k = 0; k < 3; ++k) { auto b = btn((int) ti, k).toFloat(); g.setColour(st[k] ? cc[k] : Colour(0xff45464c)); g.fillRoundedRectangle(b, 4.f);
                g.setColour(st[k] ? Colours::black : Colour(0xffbdbdc2)); g.setFont(Font(FontOptions(11.f, Font::bold))); g.drawText(lt[k], b.toNearestInt(), Justification::centred); }
        }
        g.setColour(Colour(0xff121315)); g.fillRect(hxp + head - 1, ruler, 1, H - ruler);
    }
    double beatAt(int x) const { return (x - head) / ppb; }
    void mouseDown(const MouseEvent& e) override {
        grabKeyboardFocus();
        loopDrag = 0; dragT = -1; dragMode = 0;
        if (e.y < ruler) { loopDrag = loopZone(e.getPosition()); if (loopDrag) { loopGrab = beatAt(e.x) - (loopDrag == 3 ? p.loopEnd.load() : p.loopStart.load()); return; }
            if (e.y >= ruler - 12) for (auto& mk : p.markers) { const int mx = head + (int) (mk.first * ppb); if (e.x >= mx - 2 && e.x < mx + 70) { p.beat = mk.first; return; } }
            if (e.x > hx() + head) { p.beat = jmax(0.0, std::floor(beatAt(e.x))); dragMode = 4; } return; }
        const int ti = (e.y - ruler) / laneH; if (ti >= (int) p.tracks.size()) return;
        p.selTrack = ti; p.selClip = -1; dragT = -1; dragPushed = false;
        if (e.mods.isPopupMenu()) { if (onMenu) onMenu(ti); if (onSelect) onSelect(); return; }
        if (e.x < hx() + head) { if (iconBox(ti).contains(e.getPosition())) { if (onSelect) onSelect(); repaint(); openLook(ti); return; }
            for (int k = 0; k < 3; ++k) if (hbtn(ti, k).contains(e.getPosition())) { ScopedLock sl(p.lock); auto& tr = *p.tracks[(size_t) ti];
                if (k == 0) { if (e.mods.isAltDown()) { for (auto& o : p.tracks) o->mute = false; } else tr.mute = ! tr.mute; }       
                else if (k == 1) { if (e.mods.isAltDown()) { for (auto& o : p.tracks) o->solo = false; } else tr.solo = ! tr.solo; } else tr.armed = ! tr.armed; }
            if (onSelect) onSelect(); repaint(); return; }
        const double b = beatAt(e.x); auto& cl = p.tracks[(size_t) ti]->clips;
        for (size_t i = 0; i < cl.size(); ++i) if (b >= cl[i].start && b < cl[i].start + cl[i].len) { p.selClip = (int) i; dragT = ti; dragC = (int) i; dragOff = b - cl[i].start;
            dStart = cl[i].start; dLen = cl[i].len; dOffs = cl[i].offset; dNotes = cl[i].notes; const double edge = jmin(7.0 / ppb, cl[i].len * 0.3);
            dragMode = cl[i].rec ? 1 : (b - cl[i].start < edge ? 2 : (cl[i].start + cl[i].len - b < edge ? 3 : 1)); }
        if (onSelect) onSelect(); repaint();
    }
    void mouseDoubleClick(const MouseEvent& e) override {   
        if (e.y >= ruler && e.x < hx() + head) { const int hi = (e.y - ruler) / laneH; if (hi < (int) p.tracks.size() && ! iconBox(hi).contains(e.getPosition()) && ! hbtn(hi, 0).contains(e.getPosition()) && ! hbtn(hi, 1).contains(e.getPosition()) && ! hbtn(hi, 2).contains(e.getPosition()) && onRename) onRename(hi); return; }
        const int ti = (e.y - ruler) / laneH; if (e.y < ruler || e.x < hx() + head || ti >= (int) p.tracks.size() || p.selClip >= 0 || p.tracks[(size_t) ti]->audio) return;
        ScopedLock sl(p.lock); auto& cl = p.tracks[(size_t) ti]->clips;
        const double bp = jmax(1, p.beatsPerBar.load()); p.pushUndo(); cl.push_back({ std::floor(beatAt(e.x) / bp) * bp, bp, {} }); p.selTrack = ti; p.selClip = (int) cl.size() - 1;
        if (onSelect) onSelect();
    }
    void mouseDrag(const MouseEvent& e) override {
        if (loopDrag) {
            const double gr = p.regionGrid(), minL = gr > 0.0 ? gr : 0.25, ls = p.loopStart, le = p.loopEnd, b = core::snapBeat(beatAt(e.x) - loopGrab, gr);
            if (loopDrag == 1) { const double ns = jmax(0.0, b); if (ns >= ls) { p.loopEnd = ns + (le - ls); p.loopStart = ns; } else { p.loopStart = ns; p.loopEnd = ns + (le - ls); } }
            else if (loopDrag == 2) p.loopStart = jlimit(0.0, jmax(0.0, le - minL), b);
            else p.loopEnd = jmax(ls + minL, b);
            repaint(); return; }
        if (dragMode == 4) { p.beat = jmax(0.0, std::floor(beatAt(e.x))); return; }
        if (dragMode == 0 || dragT < 0 || dragT >= (int) p.tracks.size()) return; ScopedLock sl(p.lock);
        { auto& cl0 = p.tracks[(size_t) dragT]->clips; if (dragC < 0 || dragC >= (int) cl0.size()) return; if (! dragPushed) { p.pushUndo(); dragPushed = true; } }
        auto& c = p.tracks[(size_t) dragT]->clips[(size_t) dragC];
        const double g = p.regionGrid(), minLen = g > 0.0 ? g : 0.0625, sps = c.wav ? c.wavSr * 60.0 / p.bpm.load() : 0.0;
        const double total = (c.wav && sps > 0.0) ? (double) c.wav->size() / sps : 1.0e9;
        if (dragMode == 1) c.start = jmax(0.0, core::snapBeat(beatAt(e.x) - dragOff, g));
        else if (dragMode == 2) {            // trim / extend from the left edge
            const double end0 = dStart + dLen, lo = c.wav ? jmax(0.0, dStart - dOffs) : 0.0;
            const double ns = jlimit(lo, jmax(lo, end0 - minLen), core::snapBeat(beatAt(e.x), g)), d = ns - dStart;
            c.start = ns; c.len = end0 - ns;
            if (c.wav) c.offset = dOffs + d;
            else { c.notes.clear(); for (auto nt : dNotes) { nt.start -= d; if (nt.start + nt.len <= 0.0) continue; if (nt.start < 0.0) { nt.len += nt.start; nt.start = 0.0; } c.notes.push_back(nt); } }
            c.fadeIn = jmin(c.fadeIn, c.len); c.fadeOut = jmin(c.fadeOut, c.len); }
        else if (dragMode == 3) {            // trim / extend from the right edge
            const double maxLen = c.wav ? jmax(minLen, total - dOffs) : 1.0e6;
            c.len = jlimit(minLen, maxLen, core::snapBeat(beatAt(e.x), g) - dStart); c.fadeIn = jmin(c.fadeIn, c.len); c.fadeOut = jmin(c.fadeOut, c.len); }
        if (c.start + c.len + 8.0 > p.length) { p.length = c.start + c.len + 32.0; lenChanged = true; }
    }
    void splitAtPlayhead() {
        ScopedLock sl(p.lock); if (p.selTrack < 0 || p.selTrack >= (int) p.tracks.size()) return;
        const double at = p.beat.load(); auto& cl = p.tracks[(size_t) p.selTrack]->clips; const double eps = 1.0e-4;
        auto hit = [&](const Clip& c) { return ! c.rec && at > c.start + eps && at < c.start + c.len - eps; };
        int idx = (p.selClip >= 0 && p.selClip < (int) cl.size() && hit(cl[(size_t) p.selClip])) ? p.selClip : -1;
        if (idx < 0) for (int i = 0; i < (int) cl.size(); ++i) if (hit(cl[(size_t) i])) idx = i;
        if (idx < 0) return;
        p.pushUndo(); Clip a = cl[(size_t) idx], b = a; const double cut = at - a.start, len0 = a.len;
        a.len = cut; a.fadeOut = 0.0; a.fadeIn = jmin(a.fadeIn, a.len);
        b.start = at; b.len = len0 - cut; b.fadeIn = 0.0; b.fadeOut = jmin(b.fadeOut, b.len);
        if (a.wav) b.offset = a.offset + cut;
        a.notes.clear(); b.notes.clear();
        for (auto nt : cl[(size_t) idx].notes) {
            if (nt.start < cut) { Note x = nt; x.len = jmin(x.len, cut - x.start); a.notes.push_back(x); }
            if (nt.start + nt.len > cut) { nt.len = nt.start + nt.len - jmax(nt.start, cut); nt.start = jmax(0.0, nt.start - cut); b.notes.push_back(nt); } }
        cl[(size_t) idx] = a; cl.push_back(b); p.selClip = (int) cl.size() - 1;
    }
    void zoomAt(int x, double factor) {
        const double bt = beatAt(x), np = jlimit(2.0, 400.0, ppb * factor); if (std::abs(np - ppb) < 1.0e-6) return;
        auto* v = findParentComponentOfClass<Viewport>(); const int vx = v != nullptr ? v->getViewPositionX() : 0;
        ppb = np; if (onZoom) onZoom();
        if (v != nullptr) v->setViewPosition(jmax(0, head + (int) (bt * ppb) - (x - vx)), v->getViewPositionY());
    }
    void mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& w) override {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isAltDown()) { zoomAt(e.x, std::exp((double) (w.deltaY != 0.f ? w.deltaY : w.deltaX) * 1.2)); return; }
        Component::mouseWheelMove(e, w); }
    void mouseMagnify(const MouseEvent& e, float scale) override { zoomAt(e.x, (double) scale); }
    bool keyPressed(const KeyPress& k) override {
        if (onCmd) { const auto md = k.getModifiers();
            if (k.isKeyCode(KeyPress::leftKey) && md.isAltDown()) { onCmd(454); return true; } if (k.isKeyCode(KeyPress::rightKey) && md.isAltDown()) { onCmd(455); return true; }
            if (k.isKeyCode(KeyPress::upKey) && ! md.isAnyModifierKeyDown()) { onCmd(493); return true; } if (k.isKeyCode(KeyPress::downKey) && ! md.isAnyModifierKeyDown()) { onCmd(494); return true; } }
        if (k == KeyPress::spaceKey) return false;
        if ((k == KeyPress::deleteKey || k == KeyPress::backspaceKey) && p.selected()) {
            ScopedLock sl(p.lock); auto& cl = p.tracks[(size_t) p.selTrack]->clips; p.pushUndo(); cl.erase(cl.begin() + p.selClip);
            p.selClip = -1; if (onSelect) onSelect(); return true; }
        if (k.getTextCharacter() == 'd' && p.selected()) {
            ScopedLock sl(p.lock); auto& cl = p.tracks[(size_t) p.selTrack]->clips; Clip c = cl[(size_t) p.selClip]; c.start += c.len; p.pushUndo(); cl.push_back(c); return true; }
        if (k.getTextCharacter() == 'q' && p.selected()) { ScopedLock sl(p.lock); p.pushUndo(); for (auto& n : p.selected()->notes) n.start = std::round(n.start * 4) / 4; return true; }   
        return false;
    }
};
struct ArrViewport : Viewport {
    void visibleAreaChanged(const Rectangle<int>&) override { if (auto* c = getViewedComponent()) c->repaint(); }
};
struct PianoRoll : Component, Timer {
    Project& p; static constexpr int keyW = 58, toolH = 36, rulerH = 22, velH = 70;
    std::function<void()> onChange;                                            
    float rowH = 17.f, scrollY = 0.f, zoomX = 1.f; double scrollX = 0.0;       
    int sel = -1, hoverPitch = -1, dragMode = 0, auditioned = -1, lastTrack = -2, lastClip = -2;      
    double hoverBeat = 0, grabOff = 0, auditionStop = 0; bool hoverGrid = false, hoverKey = false, hoverOnNote = false, hoverEdge = false, undoPushed = false, needFit = true;
    Track* auditionTrack = nullptr; Rectangle<int> chipGrid, chipLen, chipVel, btnOut, btnIn, btnFit;
    explicit PianoRoll(Project& pr) : p(pr) { setWantsKeyboardFocus(true); startTimerHz(20); }
    ~PianoRoll() override { stopAudition(); }
    static bool isBlack(int pitch) { const int m = pitch % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; }
    Rectangle<int> gridR() const { return { keyW, toolH + rulerH, jmax(1, getWidth() - keyW), jmax(1, getHeight() - toolH - rulerH - velH) }; }
    Rectangle<int> velR() const { return { keyW, getHeight() - velH, jmax(1, getWidth() - keyW), velH }; }
    static float rows() { return (float) (Project::hi - Project::lo); }
    float rowPx() const { return jmax(rowH, (float) gridR().getHeight() / rows()); }
    double ppb(const Clip& c) const { return (double) gridR().getWidth() / jmax(0.25, c.len) * (double) zoomX; }
    float yOf(int pitch) const { return (float) gridR().getY() + (float) (Project::hi - 1 - pitch) * rowPx() - scrollY; }
    float xOf(double beat, const Clip& c) const { return (float) keyW + (float) ((beat - scrollX) * ppb(c)); }
    double beatAt(float x, const Clip& c) const { return (double) (x - (float) keyW) / ppb(c) + scrollX; }
    int pitchAt(float y) const { return Project::hi - 1 - (int) std::floor((y - (float) gridR().getY() + scrollY) / rowPx()); }
    Rectangle<float> noteRect(const Clip& c, const Note& n) const { return { xOf(n.start, c), yOf(n.pitch) + 1.f, jmax(4.f, (float) (n.len * ppb(c))), rowPx() - 2.f }; }
    void clampScroll(const Clip* c) {
        const auto gr = gridR(); scrollY = jlimit(0.f, jmax(0.f, rows() * rowPx() - (float) gr.getHeight()), scrollY);
        if (c) scrollX = jlimit(0.0, jmax(0.0, c->len - (double) gr.getWidth() / ppb(*c)), scrollX); else scrollX = 0.0; }
    int hitNote(const Clip& c, float x, float y, bool& edge) const {
        for (int i = (int) c.notes.size() - 1; i >= 0; --i) { const auto r = noteRect(c, c.notes[(size_t) i]); if (r.contains(x, y)) { edge = x > r.getRight() - jmin(7.f, r.getWidth() * 0.4f); return i; } }
        edge = false; return -1; }
    static String noteLabel(int pitch) { return String(core::noteName(pitch)) + String(core::noteOctave(pitch)); }
    void audition(int pitch) {
        stopAudition(); if (p.selTrack < 0 || p.selTrack >= (int) p.tracks.size()) return; auto* t = p.tracks[(size_t) p.selTrack].get(); if (t->audio) return;
        auditioned = pitch; auditionTrack = t; if (t->plugin) t->coll.addMessageToQueue(MidiMessage::noteOn(1, pitch, (uint8) 96)); else t->live[pitch] = true; }
    void stopAudition() {
        if (auditioned >= 0 && auditionTrack != nullptr) { if (auditionTrack->plugin) auditionTrack->coll.addMessageToQueue(MidiMessage::noteOff(1, auditioned)); else auditionTrack->live[auditioned] = false; }
        auditioned = -1; auditionTrack = nullptr; }
    void fitToNotes() {
        auto* c = p.selected(); const float rh = rowPx(); const auto gr = gridR(); int a = 60, b = 60;
        if (c && ! c->notes.empty()) { a = Project::hi; b = Project::lo; for (auto& n : c->notes) { a = jmin(a, n.pitch); b = jmax(b, n.pitch); } }
        scrollY = (float) (Project::hi - 1 - (a + b) / 2) * rh - (float) gr.getHeight() * 0.5f + rh * 0.5f; clampScroll(c); }
    void timerCallback() override {
        { ScopedLock sl(p.lock);
          if (p.selTrack != lastTrack || p.selClip != lastClip) { lastTrack = p.selTrack; lastClip = p.selClip; sel = -1; zoomX = 1.f; scrollX = 0.0; needFit = true; }
          if (needFit && getHeight() > toolH + rulerH + velH + 40) { fitToNotes(); needFit = false; } }
        if (auditioned >= 0 && dragMode == 0 && Time::getMillisecondCounterHiRes() > auditionStop) stopAudition();      
        if (isShowing()) repaint(); }
    void resized() override {
        const int h = 22, y = (toolH - h) / 2; int x = 12 + 14 + (getWidth() > 800 ? 140 : 0);
        chipGrid = { x, y, 104, h }; x += 108; chipLen = { x, y, 112, h }; x += 116; chipVel = { x, y, 112, h };
        if (getWidth() >= 520) { int rx = getWidth() - 10; btnFit = { rx - 38, y, 38, h }; rx -= 42; btnIn = { rx - 26, y, 26, h }; rx -= 28; btnOut = { rx - 26, y, 26, h }; }
        else btnFit = btnIn = btnOut = {};
        needFit = true; }
    void drawChip(Graphics& g, Rectangle<int> r, const String& label, const String& value) {
        const auto f = r.toFloat(); g.setColour(Colour(0xff3a3b41)); g.fillRoundedRectangle(f, 5.f); g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(f.reduced(0.5f), 5.f, 1.f);
        g.setFont(Font(FontOptions(11.f))); g.setColour(Colour(0xff9a9ba3)); g.drawText(label, r.withTrimmedLeft(8).withWidth(46), Justification::centredLeft, false);
        g.setFont(Font(FontOptions(12.f, Font::bold))); g.setColour(Colour(0xfff0f0f4)); g.drawText(value, r.withTrimmedLeft(52).withTrimmedRight(16), Justification::centredLeft, false);
        Path c; c.startNewSubPath(f.getRight() - 14.f, f.getCentreY() - 1.5f); c.lineTo(f.getRight() - 10.f, f.getCentreY() + 2.f); c.lineTo(f.getRight() - 6.f, f.getCentreY() - 1.5f); g.setColour(Colour(0xff9a9ba3)); g.strokePath(c, PathStrokeType(1.4f)); }
    void drawSmall(Graphics& g, Rectangle<int> r, const String& text) {
        if (r.isEmpty()) return; g.setColour(Colour(0xff3a3b41)); g.fillRoundedRectangle(r.toFloat(), 5.f); g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.f, 1.f);
        g.setColour(Colour(0xfff0f0f4)); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText(text, r, Justification::centred, false); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff18191c)); auto* c = p.selected(); const auto gr = gridR(), vr = velR(); const float rh = rowPx();
        const bool hasTrack = p.selTrack >= 0 && p.selTrack < (int) p.tracks.size(); const Colour col = hasTrack ? p.tracks[(size_t) p.selTrack]->colour : Colour(0xff8f9097);
        g.setColour(Colour(0xff2a2b2f)); g.fillRect(0, 0, getWidth(), toolH); g.setColour(Colour(0xff0f1012)); g.fillRect(0, toolH - 1, getWidth(), 1);
        g.setColour(col); g.fillEllipse(12.f, (float) toolH * 0.5f - 4.f, 8.f, 8.f);
        if (getWidth() > 800 && hasTrack) { g.setColour(Colour(0xffe8e8ec)); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText(p.tracks[(size_t) p.selTrack]->name, 26, 0, 128, toolH, Justification::centredLeft, true); }
        static const char* grids[5] = { "Off", "1/4", "1/8", "1/16", "1/32" }; static const char* lens[4] = { "1/16", "1/8", "1/4", "1/2" };
        drawChip(g, chipGrid, "Grid", grids[jlimit(0, 4, p.noteSnap)]); drawChip(g, chipLen, "Length", lens[jlimit(0, 3, p.noteLenIdx)]); drawChip(g, chipVel, "Velocity", String(p.defVel));
        drawSmall(g, btnOut, String::charToString((juce_wchar) 0x2212)); drawSmall(g, btnIn, "+"); drawSmall(g, btnFit, "Fit");
        if (! c) {
            g.setColour(Colour(0xff8f9097)); g.setFont(Font(FontOptions(17.f, Font::bold))); g.drawText(p.tracks.empty() ? "No tracks" : "No region selected", getLocalBounds().withTrimmedTop(toolH).translated(0, -12), Justification::centred, false);
            g.setColour(Colour(0xff6b6c73)); g.setFont(Font(FontOptions(12.5f))); g.drawText(p.tracks.empty() ? "Add an instrument or audio track to start writing notes." : "Double-click a track lane in the arrangement to create a region, or click one to edit it.", getLocalBounds().withTrimmedTop(toolH).translated(0, 14), Justification::centred, false); return; }
        const double ppbv = ppb(*c), left = scrollX, right = scrollX + (double) gr.getWidth() / ppbv; const int bpb = jmax(1, p.beatsPerBar.load());
        { Graphics::ScopedSaveState ss(g); g.reduceClipRegion(gr);
          for (int pitch = Project::lo; pitch < Project::hi; ++pitch) {
              const float y = yOf(pitch); if (y + rh < (float) gr.getY() || y > (float) gr.getBottom()) continue;
              g.setColour(isBlack(pitch) ? Colour(0xff1c1d20) : Colour(0xff222327)); g.fillRect((float) gr.getX(), y, (float) gr.getWidth(), rh);
              if (pitch == hoverPitch && (hoverGrid || hoverKey)) { g.setColour(Colour(0x0fffffff)); g.fillRect((float) gr.getX(), y, (float) gr.getWidth(), rh); }
              g.setColour(pitch % 12 == 0 ? Colour(0xff40414a) : Colour(0xff2a2b30)); g.fillRect((float) gr.getX(), y + rh - 1.f, (float) gr.getWidth(), 1.f); }
          const double step = p.noteGrid() > 0.0 ? p.noteGrid() : 0.25;                                                    
          if (step * ppbv >= 7.0) { g.setColour(Colour(0x0dffffff)); for (double b = std::floor(left / step) * step; b <= right; b += step) g.fillRect(xOf(b, *c), (float) gr.getY(), 1.f, (float) gr.getHeight()); }
          for (long long ab = (long long) std::ceil(c->start + left); ab <= (long long) std::floor(c->start + right); ++ab) {      
              const bool bar = core::isDownbeat(ab, bpb); g.setColour(bar ? Colour(0x40ffffff) : Colour(0x1affffff)); g.fillRect(xOf((double) ab - c->start, *c), (float) gr.getY(), 1.f, (float) gr.getHeight()); }
          const float endX = xOf(c->len, *c); if (endX < (float) gr.getRight()) { g.setColour(Colour(0x73000000)); g.fillRect(jmax((float) gr.getX(), endX), (float) gr.getY(), (float) gr.getRight() - jmax((float) gr.getX(), endX), (float) gr.getHeight()); g.setColour(Colour(0x66ffffff)); g.fillRect(endX, (float) gr.getY(), 1.f, (float) gr.getHeight()); }
          if (hoverGrid && ! hoverOnNote && dragMode == 0 && hoverPitch >= 0) {                                                
              const double gs = core::floorBeat(jmax(0.0, hoverBeat), p.noteGrid()); Rectangle<float> gh(xOf(gs, *c), yOf(hoverPitch) + 1.f, jmax(4.f, (float) (p.noteLength() * ppbv)), rh - 2.f);
              g.setColour(col.withAlpha(0.22f)); g.fillRoundedRectangle(gh, 3.f); g.setColour(col.withAlpha(0.6f)); g.drawRoundedRectangle(gh, 3.f, 1.f); }
          for (int i = 0; i < (int) c->notes.size(); ++i) { auto& n = c->notes[(size_t) i]; const auto r = noteRect(*c, n);
              if (r.getRight() < (float) gr.getX() || r.getX() > (float) gr.getRight() || r.getBottom() < (float) gr.getY() || r.getY() > (float) gr.getBottom()) continue;
              const Colour base = col.withMultipliedBrightness(0.55f + 0.5f * (float) n.vel / 127.f); const bool s = i == sel;
              g.setColour(Colour(0x55000000)); g.fillRoundedRectangle(r.translated(0.f, 1.f), 3.f);
              g.setGradientFill(ColourGradient(base.brighter(0.25f), 0.f, r.getY(), base.darker(0.15f), 0.f, r.getBottom(), false)); g.fillRoundedRectangle(r, 3.f);
              g.setColour(s ? Colours::white : base.darker(0.55f)); g.drawRoundedRectangle(r.reduced(0.5f), 3.f, s ? 1.6f : 1.f);
              if (r.getWidth() > 12.f) { g.setColour(Colour(0x40000000)); g.fillRoundedRectangle(r.getRight() - 4.f, r.getY() + 3.f, 2.f, r.getHeight() - 6.f, 1.f); }          
              if (r.getWidth() > 34.f && rh >= 13.f) { g.setColour(Colour(0xb0000000)); g.setFont(Font(FontOptions(jmin(11.f, rh - 5.f), Font::bold))); g.drawText(noteLabel(n.pitch), r.withTrimmedLeft(5.f).withTrimmedRight(7.f), Justification::centredLeft, false); } } }
        { Graphics::ScopedSaveState ss(g); g.reduceClipRegion(0, gr.getY(), keyW, gr.getHeight());
          for (int pitch = Project::lo; pitch < Project::hi; ++pitch) {
              const float y = yOf(pitch); if (y + rh < (float) gr.getY() || y > (float) gr.getBottom()) continue; const bool black = isBlack(pitch), lit = pitch == auditioned || pitch == hoverPitch, press = pitch == auditioned;
              g.setColour(press ? col.brighter(0.2f) : lit ? Colour(0xffd9dae3) : Colour(0xffeeeef1)); g.fillRect(0.f, y, (float) keyW - 1.f, rh);
              if (black) { g.setColour(press ? col.darker(0.1f) : lit ? Colour(0xff45464d) : Colour(0xff2a2b2f)); g.fillRoundedRectangle(-4.f, y + 0.5f, (float) keyW * 0.62f + 4.f, rh - 1.f, 3.f); }
              if (pitch % 12 == 0 || pitch % 12 == 5) { g.setColour(Colour(0xffb4b5bb)); g.fillRect(0.f, y + rh - 1.f, (float) keyW - 1.f, 1.f); }
              if (pitch % 12 == 0) { g.setColour(press ? Colours::white : Colour(0xff606167)); g.setFont(Font(FontOptions(10.5f, Font::bold))); g.drawText(noteLabel(pitch), keyW - 30, (int) y, 26, (int) rh, Justification::centredRight, false); } }
          g.setColour(Colour(0xff0c0d0f)); g.fillRect(keyW - 1, gr.getY(), 1, gr.getHeight()); }
        { g.setColour(Colour(0xff25262a)); g.fillRect(0, toolH, getWidth(), rulerH); g.setColour(Colour(0xff0f1012)); g.fillRect(0, toolH + rulerH - 1, getWidth(), 1);
          Graphics::ScopedSaveState ss(g); g.reduceClipRegion(keyW, toolH, getWidth() - keyW, rulerH);
          for (long long ab = (long long) std::ceil(c->start + left); ab <= (long long) std::floor(c->start + right); ++ab) {
              const float x = xOf((double) ab - c->start, *c); const bool bar = core::isDownbeat(ab, bpb); int bn, bt, tk; core::barBeat((double) ab, bpb, bn, bt, tk);
              g.setColour(bar ? Colour(0xffa0a1a9) : Colour(0xff5a5b62)); g.fillRect(x, (float) (bar ? toolH + 3 : toolH + rulerH - 7), 1.f, (float) (bar ? rulerH - 4 : 6));
              if (bar) { g.setColour(Colour(0xffe0e0e6)); g.setFont(Font(FontOptions(11.f, Font::bold))); g.drawText(String(bn), (int) x + 4, toolH + 2, 40, 14, Justification::centredLeft, false); }
              else if (ppbv >= 56.0) { g.setColour(Colour(0xff7d7e85)); g.setFont(Font(FontOptions(9.5f))); g.drawText(String(bn) + "." + String(bt), (int) x + 3, toolH + 5, 36, 12, Justification::centredLeft, false); } } }
        { g.setColour(Colour(0xff25262a)); g.fillRect(0, toolH, keyW, rulerH); g.setColour(Colour(0xff0f1012)); g.fillRect(0, toolH + rulerH - 1, keyW, 1); g.fillRect(keyW - 1, toolH, 1, rulerH);
          g.setColour(Colour(0xff7d7e85)); g.setFont(Font(FontOptions(10.f, Font::bold))); g.drawText("BAR", 0, toolH, keyW - 4, rulerH, Justification::centredRight, false); }
        { g.setColour(Colour(0xff1b1c1f)); g.fillRect(vr); g.setColour(Colour(0xff25262a)); g.fillRect(0, vr.getY(), keyW, vr.getHeight()); g.setColour(Colour(0xff0f1012)); g.fillRect(0, vr.getY(), getWidth(), 1); g.fillRect(keyW - 1, vr.getY(), 1, vr.getHeight());
          g.setColour(Colour(0xff7d7e85)); g.setFont(Font(FontOptions(10.f, Font::bold))); g.drawText("VELOCITY", 0, vr.getY() + 6, keyW - 4, 14, Justification::centredRight, false);
          Graphics::ScopedSaveState ss(g); g.reduceClipRegion(vr); const float top = (float) vr.getY() + 6.f, bot = (float) vr.getBottom() - 4.f;
          for (int k = 1; k <= 3; ++k) { g.setColour(Colour(0x0cffffff)); g.fillRect((float) vr.getX(), bot - (bot - top) * (float) k / 4.f, (float) vr.getWidth(), 1.f); }
          for (int i = 0; i < (int) c->notes.size(); ++i) { auto& n = c->notes[(size_t) i]; const float x = xOf(n.start, *c); if (x < (float) vr.getX() - 6.f || x > (float) vr.getRight()) continue;
              const float h = jmax(3.f, (bot - top) * (float) n.vel / 127.f); const bool s = i == sel; g.setColour((s ? Colours::white : col).withAlpha(s ? 0.95f : 0.8f));
              g.fillRect(x, bot - h, 3.f, h); g.fillEllipse(x - 2.f, bot - h - 3.f, 7.f, 7.f); } }
        { const double rel = p.beat - c->start; if (rel >= left && rel <= right) { const float x = xOf(rel, *c); g.setColour(Colour(0xfff5f5f7)); g.fillRect(x, (float) toolH, 1.5f, (float) (getHeight() - toolH));
            Path t; t.addTriangle(x - 5.f, (float) toolH + 1.f, x + 6.5f, (float) toolH + 1.f, x + 0.75f, (float) toolH + 9.f); g.fillPath(t); } }
    }
    void updateHover(const MouseEvent& e) {
        ScopedLock sl(p.lock); auto* c = p.selected(); const auto gr = gridR(); const auto pt = e.getPosition();
        hoverGrid = c != nullptr && gr.contains(pt); hoverKey = c != nullptr && pt.x < keyW && pt.y >= gr.getY() && pt.y < gr.getBottom(); hoverOnNote = hoverEdge = false;
        hoverPitch = (hoverGrid || hoverKey) ? jlimit(Project::lo, Project::hi - 1, pitchAt((float) pt.y)) : -1;
        if (hoverGrid) { hoverBeat = beatAt((float) pt.x, *c); bool edge = false; hoverOnNote = hitNote(*c, (float) pt.x, (float) pt.y, edge) >= 0; hoverEdge = hoverOnNote && edge; }
        const bool chip = pt.y < toolH && (chipGrid.contains(pt) || chipLen.contains(pt) || chipVel.contains(pt) || btnOut.contains(pt) || btnIn.contains(pt) || btnFit.contains(pt));
        setMouseCursor(chip || hoverKey ? MouseCursor::PointingHandCursor : hoverEdge ? MouseCursor::LeftRightResizeCursor : hoverOnNote ? MouseCursor::DraggingHandCursor : hoverGrid ? MouseCursor::CrosshairCursor : MouseCursor::NormalCursor);
        repaint(); }
    void mouseMove(const MouseEvent& e) override { updateHover(e); }
    void mouseExit(const MouseEvent&) override { hoverGrid = hoverKey = hoverOnNote = false; hoverPitch = -1; repaint(); }
    void toolbarClick(Point<int> pt) {
        auto at = [this](Rectangle<int> r) { return PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(r)); };
        if (chipGrid.contains(pt)) { PopupMenu m; static const char* g[5] = { "Off", "1/4 note", "1/8 note", "1/16 note", "1/32 note" }; for (int i = 0; i < 5; ++i) m.addItem(1 + i, g[i], true, p.noteSnap == i);
            m.showMenuAsync(at(chipGrid), [this](int r) { if (r > 0) { p.noteSnap = r - 1; if (onChange) onChange(); repaint(); } }); }
        else if (chipLen.contains(pt)) { PopupMenu m; static const char* l[4] = { "1/16 note", "1/8 note", "1/4 note (beat)", "1/2 note" }; for (int i = 0; i < 4; ++i) m.addItem(1 + i, l[i], true, p.noteLenIdx == i);
            m.showMenuAsync(at(chipLen), [this](int r) { if (r > 0) { p.noteLenIdx = r - 1; if (onChange) onChange(); repaint(); } }); }
        else if (chipVel.contains(pt)) { PopupMenu m; static const int v[5] = { 40, 60, 80, 100, 120 }; static const char* nm[5] = { "Very soft", "Soft", "Medium", "Default", "Loud" }; for (int i = 0; i < 5; ++i) m.addItem(1 + i, String(nm[i]) + "  (" + String(v[i]) + ")", true, p.defVel == v[i]);
            m.showMenuAsync(at(chipVel), [this](int r) { if (r > 0) { p.defVel = v[r - 1]; if (onChange) onChange(); repaint(); } }); }
        else if (btnOut.contains(pt) || btnIn.contains(pt) || btnFit.contains(pt)) { ScopedLock sl(p.lock); auto* c = p.selected();
            if (btnFit.contains(pt)) { zoomX = 1.f; scrollX = 0.0; needFit = true; } else zoomX = jlimit(1.f, 32.f, btnIn.contains(pt) ? zoomX * 1.5f : zoomX / 1.5f); clampScroll(c); repaint(); } }
    void scrub(const MouseEvent& e, const Clip& c) { if (p.recClip >= 0) return; p.beat = jmax(0.0, c.start + jmax(0.0, core::snapBeat(beatAt((float) e.x, c), p.noteGrid()))); }
    void velEdit(const MouseEvent& e, bool down) {
        auto* c = p.selected(); if (! c) return; const auto vr = velR();
        if (down) { int best = -1; float bd = 9.f; for (int i = 0; i < (int) c->notes.size(); ++i) { const float d = std::abs(xOf(c->notes[(size_t) i].start, *c) + 1.5f - (float) e.x); if (d <= bd) { bd = d; best = i; } }
            if (best < 0) { dragMode = 0; return; } sel = best; dragMode = 4; p.pushUndo(); }
        if (dragMode != 4 || sel < 0 || sel >= (int) c->notes.size()) return;
        const float top = (float) vr.getY() + 6.f, bot = (float) vr.getBottom() - 4.f; c->notes[(size_t) sel].vel = jlimit(1, 127, (int) std::lround((bot - (float) e.y) / (bot - top) * 127.f)); repaint(); }
    void mouseDown(const MouseEvent& e) override {
        const auto pt = e.getPosition(); dragMode = 0; undoPushed = false;
        if (pt.y < toolH) { toolbarClick(pt); return; }
        ScopedLock sl(p.lock); auto* c = p.selected(); if (! c) return; const auto gr = gridR(), vr = velR();
        if (pt.y < toolH + rulerH) { if (pt.x >= keyW) { dragMode = 5; scrub(e, *c); } return; }
        if (pt.x < keyW) { if (pt.y >= gr.getY() && pt.y < gr.getBottom()) { dragMode = 6; audition(jlimit(Project::lo, Project::hi - 1, pitchAt((float) pt.y))); repaint(); } return; }
        if (vr.contains(pt)) { velEdit(e, true); return; }
        if (! gr.contains(pt)) return;
        bool edge = false; const int i = hitNote(*c, (float) pt.x, (float) pt.y, edge); const double beat = beatAt((float) pt.x, *c), grid = p.noteGrid(); const int pitch = jlimit(Project::lo, Project::hi - 1, pitchAt((float) pt.y));
        if (e.mods.isPopupMenu() || (e.mods.isAltDown() && i >= 0)) { if (i >= 0) { p.pushUndo(); c->notes.erase(c->notes.begin() + i); sel = -1; } repaint(); return; }
        if (i >= 0) { sel = i; auto& n = c->notes[(size_t) i]; dragMode = edge ? 2 : 1; grabOff = beat - n.start; audition(n.pitch); }
        else { p.pushUndo(); undoPushed = true; Note n { core::floorBeat(jmax(0.0, beat), grid), p.noteLength(), pitch, p.defVel };
            n.start = jlimit(0.0, jmax(0.0, c->len - 0.0625), n.start); n.len = jmin(n.len, jmax(0.0625, c->len - n.start)); c->notes.push_back(n); sel = (int) c->notes.size() - 1; dragMode = 3; audition(pitch); }
        repaint(); }
    void mouseDrag(const MouseEvent& e) override {
        if (dragMode == 0) return; ScopedLock sl(p.lock); auto* c = p.selected(); if (! c) return;
        if (dragMode == 5) { scrub(e, *c); return; } if (dragMode == 4) { velEdit(e, false); return; }
        if (dragMode == 6) { const int np = jlimit(Project::lo, Project::hi - 1, pitchAt((float) e.y)); if (np != auditioned) { audition(np); repaint(); } return; }
        if (sel < 0 || sel >= (int) c->notes.size()) return; auto& n = c->notes[(size_t) sel]; const double grid = p.noteGrid(), beat = beatAt((float) e.x, *c);
        if (! undoPushed && e.getDistanceFromDragStart() > 2) { p.pushUndo(); undoPushed = true; }
        if (! undoPushed) return;
        if (dragMode == 1) { n.start = jlimit(0.0, jmax(0.0, c->len - n.len), core::snapBeat(beat - grabOff, grid)); const int np = jlimit(Project::lo, Project::hi - 1, pitchAt((float) e.y)); if (np != n.pitch) { n.pitch = np; audition(np); } }
        else if (dragMode == 2 || e.getDistanceFromDragStart() > 3) { const double minLen = grid > 0.0 ? grid : 0.0625, end = grid > 0.0 ? core::snapBeat(beat, grid) : beat; n.len = jmin(jmax(minLen, end - n.start), jmax(0.0625, c->len - n.start)); }
        repaint(); }
    void mouseUp(const MouseEvent&) override { stopAudition(); dragMode = 0; repaint(); }
    void mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& w) override {
        ScopedLock sl(p.lock); auto* c = p.selected(); if (! c) return;
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) { const double bm = beatAt((float) e.x, *c); zoomX = jlimit(1.f, 32.f, zoomX * (1.f + w.deltaY * 1.2f)); scrollX = bm - (double) (e.x - keyW) / ppb(*c); }
        else if (e.mods.isAltDown()) { rowH = jlimit(10.f, 36.f, rowH * (1.f + w.deltaY * 0.8f)); }
        else { scrollY -= w.deltaY * 320.f; scrollX -= (double) w.deltaX * 600.0 / ppb(*c); }
        clampScroll(c); updateHover(e); repaint(); }
    bool keyPressed(const KeyPress& k) override {                  
        ScopedLock sl(p.lock); auto* c = p.selected(); if (! c || sel < 0 || sel >= (int) c->notes.size()) return false; auto& n = c->notes[(size_t) sel]; const bool shift = k.getModifiers().isShiftDown(), cmd = k.getModifiers().isCommandDown();
        if (cmd) return false;
        if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey) { p.pushUndo(); c->notes.erase(c->notes.begin() + sel); sel = -1; repaint(); return true; }
        if (k.isKeyCode(KeyPress::upKey) || k.isKeyCode(KeyPress::downKey)) { p.pushUndo(); n.pitch = jlimit(Project::lo, Project::hi - 1, n.pitch + (k.isKeyCode(KeyPress::upKey) ? 1 : -1) * (shift ? 12 : 1)); audition(n.pitch); auditionStop = Time::getMillisecondCounterHiRes() + 250.0; repaint(); return true; }
        if (k.isKeyCode(KeyPress::leftKey) || k.isKeyCode(KeyPress::rightKey)) { const double st = p.noteGrid() > 0.0 ? p.noteGrid() : 0.25; p.pushUndo(); n.start = jlimit(0.0, jmax(0.0, c->len - n.len), n.start + (k.isKeyCode(KeyPress::rightKey) ? st : -st)); repaint(); return true; }
        return false; }
};
struct EqEditor : Component {
    Project& p; Track& t; int drag = -1, hover = -1, sel = -1, dragKind = 0;   // dragKind: 0 = move point, 1 = width handle
    TextButton onB { "On" }, resetB { "Reset" }, addB { "+" }; Slider widthS { Slider::LinearHorizontal, Slider::NoTextBox };
    static constexpr int W = 480, Ht = 290;
    static constexpr double kQMin = 0.3, kQMaxPeak = 18.0, kQMaxShelf = 1.5;
    static Colour bandCol(int k) { static const Colour c[6] = { Colour(0xffff6b5e), Colour(0xffffc83d), Colour(0xff5ee08a), Colour(0xff5eb5ff), Colour(0xffc58bff), Colour(0xffff8fd0) }; return c[jlimit(0, 5, k)]; }
    static String bandName(int k) { static const char* n[6] = { "Low Shelf", "Low Mid", "High Mid", "High Shelf", "Band 5", "Band 6" }; return n[jlimit(0, 5, k)]; }
    static String hzText(double f) { return f >= 1000.0 ? String(f / 1000.0, 2) + " kHz" : String((int) std::lround(f)) + " Hz"; }
    static double qMaxOf(const EqBand& b) { return b.type == 1 ? kQMaxPeak : kQMaxShelf; }
    static double qToV(const EqBand& b) { return jlimit(0.0, 1.0, std::log(jmax(kQMin, (double) b.q) / kQMin) / std::log(qMaxOf(b) / kQMin)); }
    static float vToQ(const EqBand& b, double v) { return (float) (kQMin * std::pow(qMaxOf(b) / kQMin, jlimit(0.0, 1.0, v))); }
    static double bwOct(double q) { return (2.0 / std::log(2.0)) * std::asinh(1.0 / (2.0 * jmax(0.05, q))); }     // width of a peak, in octaves
    static double qOfBwOct(double bw) { return 1.0 / (2.0 * std::sinh(jmax(0.01, bw) * std::log(2.0) / 2.0)); }
    EqEditor(Project& pr, Track& tr) : p(pr), t(tr) {
        setSize(W, Ht);
        onB.setClickingTogglesState(true); onB.setToggleState(t.eqOn, dontSendNotification); onB.setColour(TextButton::buttonColourId, Colour(0xff45464c)); onB.setColour(TextButton::buttonOnColourId, Colour(0xff35c759)); onB.setColour(TextButton::textColourOnId, Colours::black);
        onB.onClick = [this] { ScopedLock sl(p.lock); t.eqOn = onB.getToggleState(); repaint(); };
        resetB.onClick = [this] { ScopedLock sl(p.lock); t.eqReset(); sel = hover = drag = -1; addB.setEnabled(true); updateSlider(); repaint(); };
        addB.setTooltip("Add a band (up to 6)"); addB.onClick = [this] { addBand(); };
        widthS.setRange(0.0, 1.0, 0.001); widthS.setColour(Slider::trackColourId, Colour(0xff7fe08a)); widthS.setColour(Slider::thumbColourId, Colours::white);
        widthS.onValueChange = [this] { if (sel < 0 || sel >= t.nEq) return; ScopedLock sl(p.lock); t.eq[sel].q = vToQ(t.eq[sel], widthS.getValue()); repaint(); };
        addB.setEnabled(t.nEq < 6); updateSlider();
        addAndMakeVisible(onB); addAndMakeVisible(resetB); addAndMakeVisible(addB); addAndMakeVisible(widthS); }
    void updateSlider() { const bool ok = sel >= 0 && sel < t.nEq; widthS.setEnabled(ok); if (ok) widthS.setValue(qToV(t.eq[sel]), dontSendNotification); }
    void addBand() {
        ScopedLock sl(p.lock); if (t.nEq >= 6) return;
        std::vector<double> f { 20.0, 20000.0 }; for (int k = 0; k < t.nEq; ++k) f.push_back((double) t.eq[k].freq); std::sort(f.begin(), f.end());
        double best = 0.0, fc = 1000.0; for (size_t i = 0; i + 1 < f.size(); ++i) { const double g = std::log(f[i + 1] / f[i]); if (g > best) { best = g; fc = std::sqrt(f[i] * f[i + 1]); } }   // drop it into the widest gap
        t.eq[t.nEq] = EqBand((float) fc, 1.2f, 1); ++t.nEq; sel = t.nEq - 1; addB.setEnabled(t.nEq < 6); updateSlider(); repaint(); }
    void removeBand(int k) {
        ScopedLock sl(p.lock); if (k < 4 || k >= t.nEq) return;
        for (int j = k; j < t.nEq - 1; ++j) t.eq[j] = t.eq[j + 1];
        t.eq[t.nEq - 1] = EqBand(1000.f); --t.nEq; sel = hover = drag = -1; addB.setEnabled(true); updateSlider(); repaint(); }
    Rectangle<int> plot() const { return Rectangle<int>(36, 38, W - 50, Ht - 38 - 40); }
    float xOfHz(double hz) const { const auto r = plot(); return (float) r.getX() + (float) (std::log(hz / 20.0) / std::log(1000.0)) * (float) r.getWidth(); }
    double hzOfX(float x) const { const auto r = plot(); return 20.0 * std::pow(1000.0, jlimit(0.0, 1.0, (double) (x - (float) r.getX()) / (double) r.getWidth())); }
    float yOfDb(double db) const { const auto r = plot(); return (float) r.getCentreY() - (float) (db / 18.0) * (float) r.getHeight() * 0.5f; }
    double dbOfY(float y) const { const auto r = plot(); return jlimit(-15.0, 15.0, (double) ((float) r.getCentreY() - y) / ((double) r.getHeight() * 0.5) * 18.0); }
    Point<float> nodePos(int k) const { return { xOfHz(t.eq[k].freq), yOfDb(t.eq[k].gain) }; }
    int nodeAt(Point<float> pt) const { int best = -1; float bd = 14.f; for (int k = 0; k < t.nEq; ++k) { const float d = nodePos(k).getDistanceFrom(pt); if (d < bd) { bd = d; best = k; } } return best; }
    int focus() const { return drag >= 0 ? drag : hover >= 0 ? hover : sel; }
    bool hasHandles(int k) const { return k >= 0 && k < t.nEq && t.eq[k].type == 1; }
    Point<float> handlePos(int k, int side) const {   // side: -1 left, +1 right
        const double f = t.eq[k].freq, h = bwOct(t.eq[k].q) * 0.5; return { xOfHz(jlimit(20.0, 20000.0, f * std::pow(2.0, side * h))), yOfDb(t.eq[k].gain) }; }
    int handleAt(Point<float> pt) const { const int k = sel; if (! hasHandles(k)) return 0; for (int s : { -1, 1 }) if (handlePos(k, s).getDistanceFrom(pt) < 8.f) return s; return 0; }
    void resized() override { onB.setBounds(8, 8, 46, 22); resetB.setBounds(60, 8, 56, 22); addB.setBounds(122, 8, 28, 22); widthS.setBounds(W - 150, 8, 110, 22); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2a2b30)); const auto r = plot(); g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle(r.toFloat(), 4.f);
        g.setFont(Font(FontOptions(10.f))); g.setColour(Colour(0xff7d7e85)); g.drawText("Wide", W - 190, 10, 36, 18, Justification::centredRight, false); g.drawText("Sharp", W - 38, 10, 36, 18, Justification::centredLeft, false);
        for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 }) { const int x = (int) xOfHz(f); const bool major = f == 100.0 || f == 1000.0 || f == 10000.0;
            g.setColour(major ? Colour(0xff34353a) : Colour(0xff26272b)); g.drawVerticalLine(x, (float) r.getY(), (float) r.getBottom());
            if (major || f == 20.0 || f == 20000.0 || f == 50.0 || f == 500.0 || f == 5000.0) { g.setColour(Colour(0xff7d7e85)); g.drawText(f >= 1000.0 ? String((int) (f / 1000.0)) + "k" : String((int) f), x - 16, r.getBottom() + 2, 32, 12, Justification::centred, false); } }
        for (int db : { -12, -6, 0, 6, 12 }) { const int y = (int) yOfDb(db); g.setColour(db == 0 ? Colour(0xff4a4b51) : Colour(0xff2c2d31)); g.drawHorizontalLine(y, (float) r.getX(), (float) r.getRight());
            g.setColour(Colour(0xff7d7e85)); g.drawText(db > 0 ? "+" + String(db) : String(db), 2, y - 6, 30, 12, Justification::centredRight, false); }
        const bool active = t.eqOn; Path curve, fill; const float y0 = yOfDb(0.0); fill.startNewSubPath((float) r.getX(), y0);
        for (int x = 0; x <= r.getWidth(); x += 2) { const double db = jlimit(-18.0, 18.0, t.eqDb(hzOfX((float) (r.getX() + x)), p.sr)); const float y = yOfDb(db), px = (float) (r.getX() + x);
            if (x == 0) curve.startNewSubPath(px, y); else curve.lineTo(px, y); fill.lineTo(px, y); }
        fill.lineTo((float) r.getRight(), y0); fill.closeSubPath();
        { Graphics::ScopedSaveState ss(g); g.reduceClipRegion(r); g.setColour((active ? Colour(0xff7fe08a) : Colour(0xff77777d)).withAlpha(0.16f)); g.fillPath(fill);
          g.setColour(active ? Colour(0xff7fe08a) : Colour(0xff77777d)); g.strokePath(curve, PathStrokeType(2.f)); }
        const int fk = focus();
        if (hasHandles(fk)) {                       // width handles: drag them (or pinch / scroll) to make the peak narrower or wider
            const auto a = handlePos(fk, -1), b = handlePos(fk, 1), c = nodePos(fk); g.setColour(bandCol(fk).withAlpha(0.55f)); g.drawLine(a.x, c.y, b.x, c.y, 1.4f);
            for (auto h : { a, b }) { g.setColour(bandCol(fk).withAlpha(0.9f)); g.fillRoundedRectangle(h.x - 3.f, h.y - 7.f, 6.f, 14.f, 2.f); } }
        for (int k = 0; k < t.nEq; ++k) { const auto c = nodePos(k); const bool hot = k == drag || k == hover || k == sel; const float rd = hot ? 8.f : 6.5f;
            g.setColour(bandCol(k).withAlpha(active ? 1.f : 0.45f)); g.fillEllipse(c.x - rd, c.y - rd, rd * 2.f, rd * 2.f); g.setColour(k == sel ? Colours::white : Colour(0xff111214)); g.drawEllipse(c.x - rd, c.y - rd, rd * 2.f, rd * 2.f, k == sel ? 1.6f : 1.2f);
            g.setColour(Colour(0xff111214)); g.setFont(Font(FontOptions(10.f, Font::bold))); g.drawText(String(k + 1), Rectangle<float>(c.x - rd, c.y - rd, rd * 2.f, rd * 2.f).toNearestInt(), Justification::centred, false); }
        g.setFont(Font(FontOptions(12.f)));
        if (fk >= 0 && fk < t.nEq) { const auto& b = t.eq[fk]; g.setColour(bandCol(fk));
            String txt = bandName(fk) + "    " + hzText(b.freq) + "    " + (b.gain > 0.f ? "+" : "") + String(b.gain, 1) + " dB    Q " + String(b.q, 2); if (b.type == 1) txt += "  (" + String(bwOct(b.q), 2) + " oct)";
            g.drawText(txt, 10, Ht - 28, W - 20, 18, Justification::centredLeft, false); }
        else { g.setColour(Colour(0xff7d7e85)); g.drawText("Drag points.  Pinch / scroll over a point, or drag its side handles, to sharpen.  Right-click for options.", 10, Ht - 28, W - 20, 18, Justification::centredLeft, false); }
    }
    void mouseMove(const MouseEvent& e) override { const int h = nodeAt(e.position); if (h != hover) { hover = h; repaint(); } setMouseCursor(handleAt(e.position) != 0 ? MouseCursor::LeftRightResizeCursor : MouseCursor::NormalCursor); }
    void mouseExit(const MouseEvent&) override { if (hover != -1) { hover = -1; repaint(); } }
    void mouseDown(const MouseEvent& e) override {
        const int hs = handleAt(e.position);
        if (hs != 0) { drag = sel; dragKind = 1; repaint(); return; }
        const int k = nodeAt(e.position);
        if (e.mods.isPopupMenu() && k >= 0) {
            sel = k; updateSlider(); PopupMenu pm; pm.addItem(1, "Reset Band"); if (k >= 4) pm.addItem(2, "Remove Band"); Component::SafePointer<EqEditor> sp(this);
            pm.showMenuAsync(PopupMenu::Options(), [sp, k](int r) { if (sp == nullptr) return; if (r == 1) { ScopedLock sl(sp->p.lock); auto& b = sp->t.eq[k]; b.gain = 0.f; b.q = b.type == 1 ? 0.9f : 0.71f; sp->updateSlider(); sp->repaint(); } else if (r == 2) sp->removeBand(k); });
            return; }
        drag = k; dragKind = 0; if (k >= 0) { hover = k; sel = k; updateSlider(); } repaint(); }
    void mouseDrag(const MouseEvent& e) override {
        if (drag < 0 || drag >= t.nEq) return; ScopedLock sl(p.lock); auto& b = t.eq[drag];
        if (dragKind == 1) { const double hz = hzOfX(e.position.x); const double bw = 2.0 * std::abs(std::log2(hz / (double) b.freq)); b.q = (float) jlimit(kQMin, kQMaxPeak, qOfBwOct(bw)); }
        else { b.freq = (float) jlimit(20.0, 20000.0, hzOfX(e.position.x)); b.gain = (float) dbOfY(e.position.y); }
        updateSlider(); repaint(); }
    void mouseUp(const MouseEvent&) override { drag = -1; dragKind = 0; repaint(); }
    void mouseDoubleClick(const MouseEvent& e) override { const int k = nodeAt(e.position); if (k >= 0) { ScopedLock sl(p.lock); t.eq[k].gain = 0.f; t.eq[k].q = t.eq[k].type == 1 ? 0.9f : 0.71f; updateSlider(); repaint(); } }
    void changeQ(double factor) { const int k = focus(); if (k < 0 || k >= t.nEq) return; ScopedLock sl(p.lock); auto& b = t.eq[k]; b.q = (float) jlimit(kQMin, qMaxOf(b), (double) b.q * factor); if (k == sel) updateSlider(); repaint(); }
    void mouseWheelMove(const MouseEvent&, const MouseWheelDetails& w) override { changeQ(1.0 + (double) w.deltaY * 0.8); }
    void mouseMagnify(const MouseEvent&, float scale) override { if (scale > 0.f) changeQ(1.0 / (double) scale); }       // pinch in = narrower, pinch out = wider
};
struct MixerStrip : Component, Timer {
    Project& p; Track& t; Label name; std::function<void(int)> onSlot, onOpen, onOp; std::function<void()> onSel; Rectangle<int> meterR, scaleR, volBox, pkBox, eqR;
    Slider vol { Slider::LinearVertical, Slider::NoTextBox }, pan { Slider::RotaryHorizontalVerticalDrag, Slider::NoTextBox };
    Slider sendK { Slider::RotaryHorizontalVerticalDrag, Slider::NoTextBox }; Rectangle<int> sendLbl;
    TextButton m { "M" }, s { "S" }, arm { "R" }, mon { "I" }, slot[5]; float hold = 0; int holdAge = 0;
    MixerStrip(Project& pr, Track& tr) : p(pr), t(tr) {
        name.setText(t.name, dontSendNotification); name.setJustificationType(Justification::centred); name.setFont(Font(FontOptions(13.f)));
        name.setColour(Label::backgroundColourId, Colours::transparentBlack); name.setColour(Label::textColourId, Colours::white); name.addMouseListener(this, false);
        vol.setRange(0, 1.995, 0.001); vol.setSkewFactorFromMidPoint(0.35); vol.setDoubleClickReturnValue(true, 1.0); vol.setValue(t.vol, dontSendNotification);
        vol.onValueChange = [this] { ScopedLock sl(p.lock); t.vol = (float) vol.getValue(); };
        pan.setRange(-1, 1, 0.01); pan.setDoubleClickReturnValue(true, 0.0); pan.setValue(t.pan, dontSendNotification);
        pan.onValueChange = [this] { ScopedLock sl(p.lock); t.pan = (float) pan.getValue(); };
        sendK.setRange(0, 1, 0.005); sendK.setDoubleClickReturnValue(true, 0.0); sendK.setValue(t.send, dontSendNotification); sendK.onValueChange = [this] { ScopedLock sl(p.lock); t.send = (float) sendK.getValue(); };
        const Colour on[] = { Colour(0xff4c8dff), Colour(0xffffd60a), Colour(0xffff453a), Colour(0xff64d2ff) }; const Colour tx[] = { Colours::white, Colours::black, Colours::white, Colours::black };
        TextButton* bs[] = { &m, &s, &arm, &mon };
        for (int i = 0; i < 4; ++i) { bs[i]->setClickingTogglesState(true); bs[i]->setColour(TextButton::buttonColourId, Colour(0xff6a6b70)); bs[i]->setColour(TextButton::buttonOnColourId, on[i]); bs[i]->setColour(TextButton::textColourOffId, Colours::white); bs[i]->setColour(TextButton::textColourOnId, tx[i]); }
        arm.setColour(TextButton::textColourOffId, Colour(0xffff6b5e));
        m.onClick = [this] { ScopedLock sl(p.lock); if (ModifierKeys::currentModifiers.isAltDown()) { for (auto& o : p.tracks) o->mute = false; } else t.mute = m.getToggleState(); };   
        s.onClick = [this] { ScopedLock sl(p.lock); if (ModifierKeys::currentModifiers.isAltDown()) { for (auto& o : p.tracks) o->solo = false; } else t.solo = s.getToggleState(); };  
        arm.onClick = [this] { ScopedLock sl(p.lock); t.armed = arm.getToggleState(); };
        mon.onClick = [this] { ScopedLock sl(p.lock); t.monitor = mon.getToggleState(); };
        slot[0].setButtonText(t.audio ? String("Input ") + String(t.inCh + 1) : t.plugin ? t.plugin->getName().substring(0, 12) : String(Project::presets[t.preset].name));
        slot[0].setColour(TextButton::buttonColourId, t.audio ? Colour(0xff58595e) : Colour(0xff4a62b0));
        for (int i = 0; i < 4; ++i) { slot[i + 1].setButtonText(t.fxLabel(i, 7)); slot[i + 1].setColour(TextButton::buttonColourId, t.hasAmp(i) ? Colour(0xffb5651d) : t.fx[i] ? Colour(0xff3f7f5a) : Colour(0xff2f3034)); }
        for (int i = 0; i < 5; ++i) { slot[i].onClick = [this, i] { if (! loaded(i) && onSlot) onSlot(i - 1); }; slot[i].addMouseListener(this, false); addAndMakeVisible(slot[i]); }
        for (Component* c : std::initializer_list<Component*> { &name, &vol, &pan, &sendK, &m, &s, &arm, &mon }) addAndMakeVisible(c);
        mon.setVisible(t.audio);
        for (auto* c : getChildren()) { c->setWantsKeyboardFocus(false); c->setMouseClickGrabsKeyboardFocus(false); }       
        startTimerHz(30);
    }
    bool loaded(int i) const { return t.slotUsed(i - 1); }
    int slotOf(const MouseEvent& e) const { for (int i = 0; i < 5; ++i) if (e.eventComponent == &slot[i]) return i; return -1; }
    void openLook() { CallOutBox::launchAsynchronously(std::make_unique<TrackLook>(p, t), localAreaToGlobal(name.getBounds()), nullptr); }
    void openEq() { CallOutBox::launchAsynchronously(std::make_unique<EqEditor>(p, t), localAreaToGlobal(eqR), nullptr); }
    void mouseDown(const MouseEvent& e) override {
        if (onSel) onSel(); const int i = slotOf(e);
        if (i >= 0) { if (e.mods.isPopupMenu() && onSlot) onSlot(i - 1); return; }                                         
        if (e.eventComponent == this && eqR.contains(e.getPosition())) {
            if (e.mods.isPopupMenu()) { PopupMenu em; em.addItem(1, "Open EQ..."); em.addItem(2, t.eqOn ? "Bypass EQ" : "Enable EQ"); em.addItem(3, "Reset EQ");
                Component::SafePointer<MixerStrip> sp(this);
                em.showMenuAsync(PopupMenu::Options(), [sp](int r) { if (sp == nullptr || r == 0) return; ScopedLock l(sp->p.lock);
                    if (r == 1) { MessageManager::callAsync([sp] { if (sp != nullptr) sp->openEq(); }); } else if (r == 2) sp->t.eqOn = ! sp->t.eqOn;
                    else { ScopedLock sl(sp->p.lock); sp->t.eqReset(); } }); }
            else openEq();
            return; }
        if (e.eventComponent == this && pkBox.contains(e.getPosition())) { hold = 0; return; }                                
        if (e.mods.isPopupMenu() && (e.eventComponent == &name || e.eventComponent == this)) {
            PopupMenu mm; mm.addItem(1, "Rename Track..."); mm.addItem(2, "Duplicate Track"); mm.addItem(3, "Delete Track"); mm.addSeparator();
            mm.addItem(4, "Reset Fader (0 dB)"); mm.addItem(5, "Reset Pan"); mm.addItem(6, "Reset Peak Hold"); mm.addSeparator(); mm.addItem(7, "Color & Icon...");
            Component::SafePointer<MixerStrip> sp(this);
            mm.showMenuAsync(PopupMenu::Options(), [sp](int r) { if (sp == nullptr || r == 0) return;
                if (r <= 3) { if (sp->onOp) sp->onOp(r); } else if (r == 4) { ScopedLock l(sp->p.lock); sp->t.vol = 1.f; } else if (r == 5) { ScopedLock l(sp->p.lock); sp->t.pan = 0.f; } else if (r == 7) sp->openLook(); else sp->hold = 0; }); } }
    void mouseDoubleClick(const MouseEvent& e) override {
        const int i = slotOf(e); if (i >= 0 && loaded(i) && onOpen) onOpen(i - 1);                                            
        else if (e.eventComponent == &name && onOp) onOp(1); }                                                                
    void timerCallback() override {
        if (! vol.isMouseOverOrDragging()) vol.setValue(t.vol, dontSendNotification); if (! pan.isMouseOverOrDragging()) pan.setValue(t.pan, dontSendNotification); if (! sendK.isMouseOverOrDragging()) sendK.setValue(t.send, dontSendNotification);
        m.setToggleState(t.mute, dontSendNotification); s.setToggleState(t.solo, dontSendNotification); arm.setToggleState(t.armed, dontSendNotification); mon.setToggleState(t.monitor, dontSendNotification);
        const float lv = t.level.load(); if (lv > hold) { hold = lv; holdAge = 0; } else if (++holdAge > 75) hold = jmax(lv, hold * 0.97f);
        repaint(); }
    void resized() override {
        auto r = getLocalBounds().reduced(5, 6); r.removeFromTop(3);
        eqR = r.removeFromTop(34); r.removeFromTop(4);
        slot[0].setBounds(r.removeFromTop(22)); r.removeFromTop(4);
        { auto row1 = r.removeFromTop(16); r.removeFromTop(2); auto row2 = r.removeFromTop(16); const int hw = row1.getWidth() / 2;
          slot[1].setBounds(row1.removeFromLeft(hw).withTrimmedRight(1)); slot[2].setBounds(row1.withTrimmedLeft(1)); slot[3].setBounds(row2.removeFromLeft(hw).withTrimmedRight(1)); slot[4].setBounds(row2.withTrimmedLeft(1)); }
        r.removeFromTop(4); { auto sr = r.removeFromTop(22); sendK.setBounds(sr.removeFromRight(22)); sendLbl = sr.withTrimmedRight(3); }
        r.removeFromTop(6); pan.setBounds(r.removeFromTop(34).withSizeKeepingCentre(34, 34)); r.removeFromTop(13);
        name.setBounds(r.removeFromBottom(22)); r.removeFromBottom(2);
        auto ms = r.removeFromBottom(22); r.removeFromBottom(3); auto ri = r.removeFromBottom(22); r.removeFromBottom(5);
        const int hw = ms.getWidth() / 2; m.setBounds(ms.removeFromLeft(hw).reduced(1)); s.setBounds(ms.reduced(1));
        arm.setBounds(ri.removeFromLeft(hw).reduced(1)); if (t.audio) mon.setBounds(ri.reduced(1));
        auto ro = r.removeFromBottom(18); r.removeFromBottom(4); const int vw = ro.getWidth() / 2; volBox = ro.removeFromLeft(vw).reduced(1, 0); pkBox = ro.reduced(1, 0);
        vol.setBounds(r.removeFromLeft(34)); meterR = r.removeFromRight(12); r.removeFromRight(2); scaleR = r;
    }
    void paint(Graphics& g) override {
        const bool sel = p.selTrack >= 0 && p.selTrack < (int) p.tracks.size() && p.tracks[(size_t) p.selTrack].get() == &t;
        g.fillAll(sel ? Colour(0xff5b5c61) : Colour(0xff4e4f54)); g.setColour(t.colour); g.fillRect(0, 0, getWidth() - 1, 3);
        g.setColour(Colour(0xff2a2b2f)); g.drawVerticalLine(getWidth() - 1, 0.f, (float) getHeight());
        { g.setColour(Colour(0xff2b2c30)); g.fillRoundedRectangle(eqR.toFloat(), 4.f); bool act = false; for (auto& bd : t.eq) act |= std::abs(bd.gain) > 0.05f;
          if (! act) { g.setColour(t.eqOn ? Colour(0xffe2e2e6) : Colour(0xff77777d)); g.setFont(Font(FontOptions(14.f, Font::bold))); g.drawText("EQ", eqR, Justification::centred, false); }
          else { const auto in = eqR.toFloat().reduced(4.f, 5.f); Path c;
              for (float x = 0.f; x <= in.getWidth(); x += 2.f) { const double hz = 20.0 * std::pow(1000.0, (double) x / (double) in.getWidth()); const double db = jlimit(-15.0, 15.0, t.eqDb(hz, p.sr));
                  const float y = in.getCentreY() - (float) (db / 15.0) * in.getHeight() * 0.5f; if (x == 0.f) c.startNewSubPath(in.getX(), y); else c.lineTo(in.getX() + x, y); }
              g.setColour(Colour(0xff3a3b40)); g.drawHorizontalLine((int) in.getCentreY(), in.getX(), in.getRight());
              g.setColour(t.eqOn ? Colour(0xff7fe08a) : Colour(0xff77777d)); g.strokePath(c, PathStrokeType(1.6f)); } }
        { g.setColour(Colour(0xff2f3034)); g.fillRoundedRectangle(sendLbl.toFloat(), 4.f); g.setColour(t.send > 0.0005f ? Colour(0xffe2e2e6) : Colour(0xff9a9ba2)); g.setFont(Font(FontOptions(11.f, Font::bold))); g.drawText("Reverb", sendLbl, Justification::centred, false); }
        auto pos = [this](double gain) { return (float) vol.getY() + (float) vol.getPositionOfValue(jlimit(0.0, 1.995, gain)); };        
        const float yTop = pos(1.995), yBot = pos(0.0); const auto mr = Rectangle<float>((float) meterR.getX(), yTop, (float) meterR.getWidth(), jmax(1.f, yBot - yTop));
        g.setColour(Colour(0xff0f1012)); g.fillRoundedRectangle(mr, 2.f);
        const float yL = pos(t.level.load());
        if (yBot - yL > 0.5f) { ColourGradient gr(Colour(0xffff453a), 0.f, yTop, Colour(0xff30d158), 0.f, yBot, false); const double f0 = (double) ((pos(1.0) - yTop) / jmax(1.f, yBot - yTop));
            gr.addColour(jlimit(0.001, 0.9, f0), Colour(0xffffd60a)); gr.addColour(jlimit(0.002, 0.95, f0 + 0.12), Colour(0xff30d158));
            g.setGradientFill(gr); g.fillRect(mr.reduced(1.f, 0.f).withTop(yL).withBottom(yBot)); }
        if (hold > 0.001f) { g.setColour(Colours::white.withAlpha(0.85f)); g.fillRect(mr.getX() + 1.f, pos(hold) - 1.f, mr.getWidth() - 2.f, 2.f); }
        g.setFont(9.f);
        for (int db : { 6, 3, 0, -3, -6, -12, -18, -24, -36, -48 }) { const int y = (int) pos((double) Decibels::decibelsToGain((float) db)); const bool major = db == 6 || db == 0 || db == -6 || db == -12 || db == -24 || db == -48;
            g.setColour(db == 0 ? Colour(0xffe2e2e6) : Colour(0xff9a9ba2)); g.drawHorizontalLine(y, (float) vol.getX() + 1.f, (float) vol.getX() + (major ? 9.f : 5.f));
            if (major) g.drawText(String(db), scaleR.getX(), y - 5, scaleR.getWidth() - 1, 10, Justification::centredRight, false); }
        const int pv = (int) std::round(t.pan * 64.f); g.setColour(Colour(0xffd2d2d6)); g.setFont(Font(FontOptions(10.f)));
        g.drawText(pv > 0 ? "+" + String(pv) : String(pv), 0, pan.getBottom() - 1, getWidth(), 13, Justification::centred, false);
        auto box = [&](Rectangle<int> r, const String& txt, Colour c) { g.setColour(Colour(0xff2b2c30)); g.fillRoundedRectangle(r.toFloat(), 3.f); g.setColour(c); g.setFont(Font(FontOptions(11.f, Font::bold))); g.drawText(txt, r, Justification::centred, false); };
        box(volBox, t.vol < 0.0005f ? String("-inf") : String(Decibels::gainToDecibels(t.vol), 1), Colour(0xffe8e8ec));          
        box(pkBox, hold < 0.0005f ? String("-inf") : String(Decibels::gainToDecibels(hold), 1), hold > 1.f ? Colour(0xffff453a) : Colour(0xffb4b5bc));   
    }
};
struct RevSettings : Component {
    Project& p; Slider room, damp, ret;
    explicit RevSettings(Project& pr) : p(pr) {
        setSize(260, 160); Slider* sl[3] = { &room, &damp, &ret }; const double init[3] = { (double) p.revRoom.load(), (double) p.revDamp.load(), (double) p.revReturn.load() };
        for (int i = 0; i < 3; ++i) { sl[i]->setSliderStyle(Slider::LinearHorizontal); sl[i]->setTextBoxStyle(Slider::NoTextBox, true, 0, 0); sl[i]->setRange(0.0, 1.0, 0.01); sl[i]->setValue(init[i], dontSendNotification); sl[i]->getProperties().set("logic", true); addAndMakeVisible(sl[i]); }
        room.onValueChange = [this] { p.revRoom = (float) room.getValue(); }; damp.onValueChange = [this] { p.revDamp = (float) damp.getValue(); }; ret.onValueChange = [this] { p.revReturn = (float) ret.getValue(); }; }
    void resized() override { auto r = getLocalBounds().reduced(14, 0); r.removeFromTop(46); for (auto* sd : { &room, &damp, &ret }) { sd->setBounds(r.removeFromTop(28)); r.removeFromTop(6); } }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2a2b30)); g.setColour(Colours::white); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText("Reverb Bus", 14, 8, 200, 18, Justification::centredLeft, false);
        g.setFont(Font(FontOptions(11.f))); g.setColour(Colour(0xff9a9ba2)); const char* nm[3] = { "Room size", "Damping", "Return level" }; for (int i = 0; i < 3; ++i) g.drawText(nm[i], 14, 32 + i * 34, 200, 12, Justification::centredLeft, false); }
};
struct NotesBox : Component {
    Project& p; TextEditor ed;
    explicit NotesBox(Project& pr) : p(pr) { setSize(380, 280); ed.setMultiLine(true, true); ed.setReturnKeyStartsNewLine(true); ed.setText(p.notes, dontSendNotification); ed.onTextChange = [this] { p.notes = ed.getText(); }; addAndMakeVisible(ed); }
    void resized() override { ed.setBounds(getLocalBounds().reduced(8).withTrimmedTop(22)); }
    void paint(Graphics& g) override { g.fillAll(Colour(0xff2a2b30)); g.setColour(Colours::white); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText("Project Notes", 12, 6, 200, 18, Justification::centredLeft, false); }
};
struct MixerPanel : Component {
    Project& p; std::vector<std::unique_ptr<MixerStrip>> strips; std::function<void(Track&, int)> onSlot, onOpen; std::function<void()> onSelect; std::function<void(int)> onTrackOp; static constexpr int stripW = 96;
    explicit MixerPanel(Project& pr) : p(pr) { rebuild(); }
    void rebuild() {
        strips.clear();
        for (size_t i = 0; i < p.tracks.size(); ++i) { auto* tr = p.tracks[i].get(); strips.push_back(std::make_unique<MixerStrip>(p, *tr));
            strips.back()->onSlot = [this, tr](int s) { if (onSlot) onSlot(*tr, s); };
            strips.back()->onOpen = [this, tr](int s) { if (onOpen) onOpen(*tr, s); };
            strips.back()->onSel = [this, i] { p.selTrack = (int) i; p.selClip = -1; if (onSelect) onSelect(); };
            strips.back()->onOp = [this, i](int op) { p.selTrack = (int) i; p.selClip = -1; if (onTrackOp) onTrackOp(op); };
            addAndMakeVisible(*strips.back()); }
        resized();
    }
    void resized() override { auto r = getLocalBounds(); for (auto& s : strips) s->setBounds(r.removeFromLeft(stripW)); }
};
struct Inspector : Component, Timer {
    Project& p; Track* t = nullptr; std::function<void(Track&, int)> onSlot, onOpen; std::function<void()> onChange, onDelete; std::function<void(int)> onTrackOp;
    TextEditor name; TextButton del { "Delete Track" }; Slider sl[3]; std::unique_ptr<MixerStrip> strip; Rectangle<int> regionR, synthR;
    explicit Inspector(Project& pr) : p(pr) {
        name.setFont(Font(FontOptions(15.f, Font::bold))); name.setColour(TextEditor::backgroundColourId, Colour(0xff1c1d20)); name.setColour(TextEditor::outlineColourId, Colour(0xff3a3b40));
        name.onReturnKey = [] { unfocusAllComponents(); };
        name.onFocusLost = [this] { if (t && t->name != name.getText()) { { ScopedLock l(p.lock); t->name = name.getText(); } if (onChange) onChange(); } };
        del.setColour(TextButton::buttonColourId, Colour(0xff6e2b2b)); del.onClick = [this] { if (onDelete) onDelete(); };
        const char* lab[] = { "Cutoff", "Attack", "Release" }; const double lo[] = { 0, 0.001, 0.01 }, hi[] = { 1, 2, 3 };
        for (int i = 0; i < 3; ++i) { auto& s = sl[i]; s.setSliderStyle(Slider::LinearBar); s.setTextBoxStyle(Slider::TextBoxLeft, false, 160, 20); s.setTextBoxIsEditable(false);
            s.setRange(lo[i], hi[i], 0.001); if (i >= 1) s.setSkewFactorFromMidPoint(0.2); s.setWantsKeyboardFocus(false); s.setMouseClickGrabsKeyboardFocus(false);
            s.textFromValueFunction = [l = String(lab[i])](double v) { return l + "   " + String(v, 2); };
            s.onValueChange = [this, i] { if (! t) return; ScopedLock l(p.lock); const float v = (float) sl[i].getValue(); if (i == 0) t->cutoff = v; else if (i == 1) t->atk = v; else t->rel = v; };
            addAndMakeVisible(s); }
        del.setWantsKeyboardFocus(false); del.setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(name); addAndMakeVisible(del); startTimerHz(6);
    }
    void timerCallback() override { repaint(regionR); }                                       
    void refresh() {
        t = (p.selTrack >= 0 && p.selTrack < (int) p.tracks.size()) ? p.tracks[(size_t) p.selTrack].get() : nullptr; setEnabled(t != nullptr); strip.reset();
        name.setVisible(t != nullptr); del.setVisible(t != nullptr); for (auto& s : sl) s.setVisible(t != nullptr && ! t->audio);       
        if (! t) { resized(); repaint(); return; }
        name.setText(t->name, false);
        strip = std::make_unique<MixerStrip>(p, *t);
        strip->onSlot = [this](int s) { if (t && onSlot) onSlot(*t, s); }; strip->onOpen = [this](int s) { if (t && onOpen) onOpen(*t, s); }; strip->onOp = [this](int op) { if (onTrackOp) onTrackOp(op); };
        addAndMakeVisible(*strip);
        const double v[] = { t->cutoff, t->atk, t->rel }; const bool synth = ! (t->audio || t->plugin);
        for (int i = 0; i < 3; ++i) { sl[i].setValue(v[i], dontSendNotification); sl[i].setEnabled(synth); }
        resized(); repaint();
    }
    void box(Graphics& g, const Rectangle<int>& r, const String& title) {
        g.setColour(Colour(0xff26272b)); g.fillRoundedRectangle(r.toFloat(), 5.f); g.setColour(Colour(0xff8f9097)); g.setFont(Font(FontOptions(11.f, Font::bold)));
        g.drawText(title, r.getX() + 8, r.getY() + 3, r.getWidth() - 16, 14, Justification::centredLeft, false); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2f3034));
        if (! t) { g.setColour(Colour(0xff8f9097)); g.setFont(Font(FontOptions(15.f, Font::bold))); g.drawText(p.tracks.empty() ? "No tracks" : "No track selected", getLocalBounds().withTrimmedBottom(20), Justification::centred, false);
            g.setColour(Colour(0xff6b6c73)); g.setFont(Font(FontOptions(12.f))); g.drawFittedText(p.tracks.empty() ? "Add an instrument or audio track to get started." : "Click a track in the arrangement.", getLocalBounds().reduced(16, 0).withTrimmedTop(getHeight() / 2 + 4), Justification::centredTop, 3); return; }
        g.setColour(t->colour); g.fillRect(0, 0, getWidth(), 4);
        box(g, regionR, "Region"); g.setFont(Font(FontOptions(12.f)));
        { const int bpb = jmax(1, p.beatsPerBar.load());
          if (auto* c = p.selected()) { int bar, bt, tk; core::barBeat(c->start, bpb, bar, bt, tk);
              g.setColour(Colour(0xffe4e4e8)); g.drawText("Start  " + String(bar) + " . " + String(bt) + " . " + String(tk) + "     " + (c->wav ? String("Audio") : String((int) c->notes.size()) + " notes"), regionR.getX() + 8, regionR.getY() + 18, regionR.getWidth() - 12, 14, Justification::centredLeft, false);
              g.drawText("Length  " + String(c->len / bpb, 2) + " bars", regionR.getX() + 8, regionR.getY() + 33, regionR.getWidth() - 12, 14, Justification::centredLeft, false); }
          else { g.setColour(Colour(0xff7d7e85)); g.drawText("No region selected", regionR.getX() + 8, regionR.getY() + 24, regionR.getWidth() - 12, 16, Justification::centredLeft, false); } }
        if (t->audio) return;                                                                      
        box(g, synthR, t->plugin ? "Plug-in" : "Synth");
        if (t->plugin) { g.setColour(Colour(0xff7d7e85)); g.setFont(Font(FontOptions(11.f))); g.drawFittedText("Double-click the instrument slot to edit the plug-in.", synthR.reduced(8, 22), Justification::topLeft, 3); }
    }
    void resized() override {
        auto r = getLocalBounds().reduced(8); r.removeFromTop(4); name.setBounds(r.removeFromTop(28)); r.removeFromTop(8);
        regionR = r.removeFromTop(52); r.removeFromTop(8);
        auto left = r.removeFromLeft(MixerPanel::stripW); if (strip) strip->setBounds(left); r.removeFromLeft(8);
        if (t != nullptr && t->audio) synthR = {}; else { synthR = r.removeFromTop(20 + 3 * 25 + 6); auto sr = synthR.reduced(6, 0); sr.removeFromTop(20); for (auto& s : sl) { s.setBounds(sr.removeFromTop(22)); sr.removeFromTop(3); } }
        r.removeFromTop(10); del.setBounds(r.removeFromTop(28));
    }
};
struct MasterStrip : Component, Timer {                                  
    Project& p; Track& m; std::function<void(int)> onSlot, onOpen;
    Slider vol { Slider::LinearVertical, Slider::NoTextBox }, pan { Slider::RotaryHorizontalVerticalDrag, Slider::NoTextBox };
    Label name, readout; TextButton mute { "M" }, lim { "Lim" }, slot[4]; Rectangle<int> meterL, meterR, clipR, volBox; float holdL = 0, holdR = 0; int holdAge = 0;
    explicit MasterStrip(Project& pr) : p(pr), m(pr.masterTrack) {
        vol.setRange(0, 1.2, 0.001); vol.setSkewFactorFromMidPoint(0.3); vol.setDoubleClickReturnValue(true, 1.0); vol.setValue(p.master, dontSendNotification);
        vol.onValueChange = [this] { p.master = (float) vol.getValue(); };
        pan.setRange(-1, 1, 0.01); pan.setDoubleClickReturnValue(true, 0.0); pan.setValue(m.pan, dontSendNotification);
        pan.onValueChange = [this] { ScopedLock sl(p.lock); m.pan = (float) pan.getValue(); };
        TextButton* bs[] = { &mute, &lim }; const Colour on[] = { Colour(0xff4c8dff), Colour(0xff35c759) };
        for (int i = 0; i < 2; ++i) { bs[i]->setClickingTogglesState(true); bs[i]->setColour(TextButton::buttonColourId, Colour(0xff45464c)); bs[i]->setColour(TextButton::buttonOnColourId, on[i]); bs[i]->setColour(TextButton::textColourOnId, i == 0 ? Colours::white : Colours::black); }
        mute.onClick = [this] { ScopedLock sl(p.lock); m.mute = mute.getToggleState(); };
        lim.onClick = [this] { p.masterLimit = lim.getToggleState(); };
        for (int i = 0; i < 4; ++i) { slot[i].onClick = [this, i] { if (! m.fx[i] && onSlot) onSlot(i); }; slot[i].addMouseListener(this, false); addAndMakeVisible(slot[i]); }
        name.setText("Master", dontSendNotification); name.setJustificationType(Justification::centred); name.setFont(Font(FontOptions(13.f, Font::bold)));
        name.setColour(Label::backgroundColourId, Colour(0xff5a5c64)); name.setColour(Label::textColourId, Colours::white); name.addMouseListener(this, false);
        readout.setJustificationType(Justification::centred); readout.setFont(Font(FontOptions(11.f))); readout.setColour(Label::backgroundColourId, Colour(0xff0e0f11));
        readout.setColour(Label::textColourId, Colour(0xffc8c8cc)); readout.addMouseListener(this, false);        
        for (Component* c : std::initializer_list<Component*> { &vol, &pan, &mute, &lim, &name, &readout }) addAndMakeVisible(c);
        startTimerHz(30); }
    int slotOf(const MouseEvent& e) const { for (int i = 0; i < 4; ++i) if (e.eventComponent == &slot[i]) return i; return -1; }
    void resetPeaks() { holdL = holdR = 0; p.masterClip = false; }
    void mouseDown(const MouseEvent& e) override {
        const int i = slotOf(e); if (i >= 0) { if (e.mods.isPopupMenu() && onSlot) onSlot(i); return; }
        if (e.eventComponent == &readout || (e.eventComponent == this && clipR.contains(e.getPosition()))) { resetPeaks(); return; }
        if (! e.mods.isPopupMenu()) return;
        PopupMenu mm; mm.addItem(1, "Reset Fader (0 dB)"); mm.addItem(2, "Reset Balance"); mm.addItem(3, "Reset Peak Hold and Clip Light"); mm.addItem(4, "Soft Limiter", true, p.masterLimit.load());
        mm.addSeparator(); mm.addItem(9, "Delete Track  (the Master can't be deleted)", false);
        mm.showMenuAsync(PopupMenu::Options(), [this](int r) {
            if (r == 1) p.master = 1.f; else if (r == 2) { ScopedLock sl(p.lock); m.pan = 0; } else if (r == 3) resetPeaks(); else if (r == 4) p.masterLimit = ! p.masterLimit.load(); }); }
    void mouseDoubleClick(const MouseEvent& e) override { const int i = slotOf(e); if (i >= 0 && m.fx[i] && onOpen) onOpen(i); }
    void timerCallback() override {
        if (! vol.isMouseOverOrDragging()) vol.setValue(p.master, dontSendNotification); if (! pan.isMouseOverOrDragging()) pan.setValue(m.pan, dontSendNotification);
        mute.setToggleState(m.mute, dontSendNotification); lim.setToggleState(p.masterLimit.load(), dontSendNotification);
        for (int i = 0; i < 4; ++i) { slot[i].setButtonText(m.fx[i] ? m.fx[i]->getName().substring(0, 12) : String()); slot[i].setColour(TextButton::buttonColourId, m.fx[i] ? Colour(0xff3f7f5a) : Colour(0xff222326)); }
        const float l = p.masterLevelL.load(), r = p.masterLevelR.load();
        if (l > holdL || r > holdR) { holdL = jmax(holdL, l); holdR = jmax(holdR, r); holdAge = 0; } else if (++holdAge > 60) { holdL = jmax(l, holdL * 0.95f); holdR = jmax(r, holdR * 0.95f); }
        const float pk = jmax(holdL, holdR); readout.setText(pk < 0.0005f ? String("-inf") : String(Decibels::gainToDecibels(pk), 1), dontSendNotification);
        repaint(); }
    void resized() override {
        auto r = getLocalBounds().reduced(4); auto top = r.removeFromTop(22); r.removeFromTop(3);        
        clipR = top.removeFromLeft(14).withSizeKeepingCentre(10, 10); readout.setBounds(top.reduced(1, 0));   
        for (int i = 0; i < 4; ++i) { slot[i].setBounds(r.removeFromTop(16)); r.removeFromTop(1); }
        r.removeFromTop(5); pan.setBounds(r.removeFromTop(34).withSizeKeepingCentre(34, 34)); r.removeFromTop(13);
        auto b = r.removeFromTop(22); mute.setBounds(b.removeFromLeft(b.getWidth() / 2).reduced(1)); lim.setBounds(b.reduced(1));
        r.removeFromTop(4); name.setBounds(r.removeFromBottom(24)); r.removeFromBottom(3); volBox = r.removeFromBottom(18).reduced(14, 0); r.removeFromBottom(4);
        auto mr = r.removeFromRight(16).reduced(0, 4); meterL = mr.removeFromLeft(7); mr.removeFromLeft(2); meterR = mr;     
        r.removeFromRight(3); vol.setBounds(r.removeFromRight(34)); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2f3035)); g.setColour(Colour(0xff151618)); g.drawVerticalLine(0, 0.f, (float) getHeight());
        g.setColour(p.masterClip.load() ? Colour(0xffe5413b) : Colour(0xff3a1a1a)); g.fillRoundedRectangle(clipR.toFloat(), 2.f);
        auto bar = [&](Rectangle<int> r, float lv, float hold) {
            g.setColour(Colour(0xff0f1012)); g.fillRoundedRectangle(r.toFloat(), 2.f);
            auto in = r.toFloat().reduced(1.f); auto fill = in.withTop(in.getBottom() - in.getHeight() * core::meterPos(lv));
            ColourGradient gr(Colour(0xffe5413b), 0.f, (float) r.getY(), Colour(0xff35c759), 0.f, (float) r.getBottom(), false); gr.addColour(0.15, Colour(0xffe8c53a)); gr.addColour(0.3, Colour(0xff35c759));
            g.setGradientFill(gr); g.fillRect(fill);
            if (hold > 0.001f) { g.setColour(Colours::white.withAlpha(0.9f)); g.fillRect(in.getX(), in.getBottom() - in.getHeight() * core::meterPos(hold) - 1.f, in.getWidth(), 2.f); } };
        bar(meterL, p.masterLevelL.load(), holdL); bar(meterR, p.masterLevelR.load(), holdR); g.setFont(9.f);
        g.setColour(Colour(0xff151618)); g.fillRoundedRectangle(volBox.toFloat(), 3.f); g.setColour(Colour(0xffe8e8ec)); g.setFont(Font(FontOptions(11.f, Font::bold)));
        g.drawText(p.master.load() < 0.0005f ? String("-inf") : String(Decibels::gainToDecibels(p.master.load()), 1), volBox, Justification::centred, false); g.setFont(9.f);
        for (int db : { 6, 0, -6, -12, -24, -48 }) { const int y = vol.getY() + (int) vol.getPositionOfValue((double) Decibels::decibelsToGain((float) db));
            g.setColour(Colour(0xff7a7a80)); g.drawText(String(db), 2, y - 5, 22, 10, Justification::centredRight); g.drawHorizontalLine(y, (float) vol.getX() - 4.f, (float) vol.getX()); } }
};
struct PrefPage : Component {                                          
    std::vector<std::pair<String, std::unique_ptr<Component>>> rows;
    void addSlider(const String& label, double lo, double hi, double step, double v, std::function<void(double)> cb) {
        auto s = std::make_unique<Slider>(Slider::LinearHorizontal, Slider::TextBoxRight); s->setRange(lo, hi, step); s->setValue(v, dontSendNotification);
        s->onValueChange = [sp = s.get(), cb] { cb(sp->getValue()); }; addAndMakeVisible(*s); rows.emplace_back(label, std::move(s)); }
    void addText(const String& label, const String& v, std::function<void(String)> cb) {
        auto e = std::make_unique<TextEditor>(); e->setText(v, false); e->onTextChange = [ep = e.get(), cb] { cb(ep->getText()); }; addAndMakeVisible(*e); rows.emplace_back(label, std::move(e)); }
    void addChoice(const String& label, const StringArray& items, int sel, std::function<void(int)> cb) {
        auto c = std::make_unique<ComboBox>(); c->addItemList(items, 1); c->setSelectedItemIndex(jlimit(0, items.size() - 1, sel), dontSendNotification);
        c->onChange = [cp = c.get(), cb] { cb(cp->getSelectedItemIndex()); }; addAndMakeVisible(*c); rows.emplace_back(label, std::move(c)); }
    int rowCount() const { return (int) rows.size(); }
    void addToggle(const String& label, bool v, std::function<void(bool)> cb) {
        auto b = std::make_unique<ToggleButton>(); b->setToggleState(v, dontSendNotification); b->onClick = [bp = b.get(), cb] { cb(bp->getToggleState()); }; addAndMakeVisible(*b); rows.emplace_back(label, std::move(b)); }
    void paint(Graphics& g) override {                                                       
        g.fillAll(Colour(0xff25262b)); int y = 16;
        for (auto& r : rows) { g.setColour(Colour(0xff2f3036)); g.fillRoundedRectangle(14.f, (float) y, (float) getWidth() - 28.f, 40.f, 10.f); g.setColour(Colour(0x0fffffff)); g.drawRoundedRectangle(14.5f, (float) y + 0.5f, (float) getWidth() - 29.f, 39.f, 10.f, 1.f);
            g.setColour(Colour(0xffe4e4e8)); g.setFont(Font(FontOptions(14.f))); g.drawText(r.first, 30, y, 240, 40, Justification::centredLeft, true); y += 46; } }
    void resized() override { int y = 16; for (auto& r : rows) { r.second->setBounds(280, y + 5, jmax(60, getWidth() - 280 - 30), 30); y += 46; } }
};
struct ExportOpts {   // content: 1 take, 2 mixdown, 3 stems, 4 mixdown + stems, 5 selected region, 6 selected track
    int content = 2, format = 1, depth = 2, rate = 0, channels = 0, range = 0, norm = 1, tail = 2;
    bool dither = true, trim = false, skipMuted = true, reveal = true; String base;
};
struct ExportPanel : Component {
    ComboBox content, range, format, depth, rate, chans, norm, tail; TextEditor base;
    ToggleButton dither { "Dither when exporting 16-bit" }, trim { "Trim silence at the end" }, skipMuted { "Skip muted tracks (separate files)" }, reveal { "Show files when finished" };
    TextButton go { "Export..." }; std::function<void(const ExportOpts&)> onExport;
    ExportPanel(const ExportOpts& o, bool hasTake, bool hasRegion, bool hasTrack, const String& trackName) {
        if (hasTake) content.addItem("Recorded take (audio region)", 1);
        content.addItem("Project mixdown (all tracks)", 2); content.addItem("Each track as a separate file (stems)", 3); content.addItem("Mixdown + each track separately", 4);
        if (hasRegion) content.addItem("Selected region only", 5);
        if (hasTrack) content.addItem(String("Selected track only") + (trackName.isNotEmpty() ? " (" + trackName + ")" : String()), 6);
        content.setSelectedId(content.indexOfItemId(o.content) >= 0 ? o.content : 2, dontSendNotification);
        range.addItemList({ "Whole project", "Loop range", "Selected region" }, 1); range.setSelectedId(o.range + 1, dontSendNotification);
        format.addItemList({ "WAV", "AIFF", "FLAC" }, 1); format.setSelectedId(o.format, dontSendNotification);
        depth.addItemList({ "16-bit", "24-bit", "32-bit float (WAV only)" }, 1); depth.setSelectedId(o.depth, dontSendNotification);
        rate.addItemList({ "Project sample rate", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, 1); rate.setSelectedId(o.rate + 1, dontSendNotification);
        chans.addItemList({ "Stereo", "Mono" }, 1); chans.setSelectedId(o.channels + 1, dontSendNotification);
        norm.addItemList({ "Off", "Peak to -1 dB", "Peak to -0.1 dB" }, 1); norm.setSelectedId(o.norm + 1, dontSendNotification);
        tail.addItemList({ "No tail", "1 second", "2 seconds", "4 seconds", "8 seconds" }, 1); tail.setSelectedId(o.tail + 1, dontSendNotification);
        base.setMultiLine(false); base.setText(o.base, false);
        dither.setToggleState(o.dither, dontSendNotification); trim.setToggleState(o.trim, dontSendNotification); skipMuted.setToggleState(o.skipMuted, dontSendNotification); reveal.setToggleState(o.reveal, dontSendNotification);
        content.onChange = [this] { const int id = content.getSelectedId(); range.setEnabled(id != 1 && id != 5); tail.setEnabled(id != 1); };
        go.onClick = [this] { if (! onExport) return; ExportOpts o2; o2.content = content.getSelectedId(); o2.range = range.getSelectedId() - 1; o2.format = format.getSelectedId(); o2.depth = depth.getSelectedId();
            o2.rate = rate.getSelectedId() - 1; o2.channels = chans.getSelectedId() - 1; o2.norm = norm.getSelectedId() - 1; o2.tail = tail.getSelectedId() - 1;
            o2.dither = dither.getToggleState(); o2.trim = trim.getToggleState(); o2.skipMuted = skipMuted.getToggleState(); o2.reveal = reveal.getToggleState(); o2.base = base.getText().trim(); if (o2.base.isEmpty()) o2.base = "MiniDAW"; onExport(o2); };
        for (Component* c : std::initializer_list<Component*> { &content, &range, &format, &depth, &rate, &chans, &norm, &tail, &base, &dither, &trim, &skipMuted, &reveal, &go }) addAndMakeVisible(c);
        content.onChange();
    }
    void paint(Graphics& g) override { g.fillAll(Colour(0xff2b2d31)); g.setColour(Colours::lightgrey); g.setFont(14.f);
        const char* l[] = { "Content", "Range", "Format", "Bit depth", "Sample rate", "Channels", "Normalize", "Tail", "File name" }; for (int i = 0; i < 9; ++i) g.drawText(l[i], 16, 16 + i * 36, 100, 28, Justification::centredLeft); }
    void resized() override { auto r = getLocalBounds().reduced(16);
        for (Component* c : std::initializer_list<Component*> { &content, &range, &format, &depth, &rate, &chans, &norm, &tail, &base }) { auto row = r.removeFromTop(28); row.removeFromLeft(110); c->setBounds(row); r.removeFromTop(8); }
        for (auto* t : { &dither, &trim, &skipMuted, &reveal }) t->setBounds(r.removeFromTop(24));
        r.removeFromTop(10); go.setBounds(r.removeFromTop(32).removeFromRight(140)); }
};
struct ExportProgress : Component {
    double prog = 0.0; ProgressBar bar { prog }; Label msg;
    ExportProgress() { setSize(400, 96); msg.setJustificationType(Justification::centredLeft); msg.setColour(Label::textColourId, Colours::white); addAndMakeVisible(msg); addAndMakeVisible(bar); }
    void set(double v, const String& m) { prog = v; msg.setText(m, dontSendNotification); repaint(); }
    void resized() override { auto r = getLocalBounds().reduced(16); msg.setBounds(r.removeFromTop(24)); r.removeFromTop(10); bar.setBounds(r.removeFromTop(20)); }
};
struct ExportJobSpec { String name; int solo; };   // solo: track index, -1 = full mix, -2 = recorded take
struct ExportState {
    ExportOpts o; File dir, single; std::vector<ExportJobSpec> jobs; size_t idx = 0; bool rendering = false; double b0 = 0.0, b1 = 4.0, tail = 2.0; StringArray written, errors; String ext;
    Component::SafePointer<ExportProgress> prog; Component::SafePointer<Component> win;
};
struct PluginWindow : DocumentWindow {
    PluginWindow(Component* e, const String& n) : DocumentWindow(n, Colours::darkgrey, closeButton) {
        setUsingNativeTitleBar(true); setContentOwned(e, true); centreWithSize(getWidth(), getHeight()); setVisible(true); }
    void closeButtonPressed() override { setVisible(false); }
};
struct IconButton : Button {                                  
    enum Kind { Rewind, Stop, Play, Rec, Cycle, Metro, Inspector, Mixer, Editor, Tuner };
    Kind kind;
    explicit IconButton(Kind k) : Button(String()), kind(k) { setMouseCursor(MouseCursor::PointingHandCursor); }
    void paintButton(Graphics& g, bool over, bool down) override {
        auto b = getLocalBounds().toFloat().reduced(1.f, 1.f); const bool on = getToggleState();
        Colour bg = Colours::transparentBlack, ic = Colour(0xffd6d7dc);
        switch (kind) {
            case Play:      if (on) { bg = Colour(0xff32d74b); ic = Colour(0xff10260f); } break;
            case Rec:       ic = Colour(0xffff453a); if (on) { bg = Colour(0xffff453a); ic = Colours::white; } break;
            case Cycle:
            case Metro:     if (on) { bg = Colour(0xffffd60a); ic = Colour(0xff2a2300); } break;
            case Inspector:
            case Mixer:
            case Editor:
            case Tuner:     if (on) { bg = Colour(0xffd2d3d8); ic = Colour(0xff1c1c1e); } break;
            case Rewind: case Stop: default: break;
        }
        if (bg.isTransparent()) { if (down) bg = Colour(0x33ffffff); else if (over) bg = Colour(0x16ffffff); }
        else if (down) bg = bg.darker(0.25f); else if (over) bg = bg.brighter(0.1f);
        if (! bg.isTransparent()) { g.setColour(bg); g.fillRoundedRectangle(b, 6.f); }
        g.setColour(ic);
        auto r = b.withSizeKeepingCentre(14.f, 14.f); const float cx = r.getCentreX(), cy = r.getCentreY(); Path p;
        switch (kind) {
            case Stop: p.addRoundedRectangle(r.reduced(1.f), 2.f); g.fillPath(p); break;
            case Play: p.addTriangle(r.getX() + 2.f, r.getY(), r.getX() + 2.f, r.getBottom(), r.getRight(), cy); g.fillPath(p); break;
            case Rec:  g.fillEllipse(r.reduced(0.5f)); break;
            case Rewind: g.fillRect(r.getX(), r.getY(), 2.f, r.getHeight());
                p.addTriangle(r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + 4.f, cy); g.fillPath(p); break;
            case Cycle: { const float rad = 6.f, a1 = MathConstants<float>::twoPi - 0.7f; p.addCentredArc(cx, cy, rad, rad, 0.f, 0.7f, a1, true); g.strokePath(p, PathStrokeType(1.8f));
                const float ex = cx + rad * std::sin(a1), ey = cy - rad * std::cos(a1), tx = std::cos(a1), ty = std::sin(a1), nx = std::sin(a1), ny = -std::cos(a1); Path h;
                h.addTriangle(ex + tx * 4.f, ey + ty * 4.f, ex + nx * 3.2f, ey + ny * 3.2f, ex - nx * 3.2f, ey - ny * 3.2f); g.fillPath(h); break; }
            case Metro: p.startNewSubPath(cx - 5.f, r.getBottom()); p.lineTo(cx - 2.f, r.getY()); p.lineTo(cx + 2.f, r.getY()); p.lineTo(cx + 5.f, r.getBottom()); p.closeSubPath();
                g.strokePath(p, PathStrokeType(1.5f)); g.drawLine(cx, r.getBottom() - 2.f, cx + 3.f, r.getY() + 2.5f, 1.5f); break;
            case Inspector: g.drawRoundedRectangle(r, 2.f, 1.5f); g.fillRect(r.getX() + 1.5f, r.getY() + 1.5f, 3.5f, r.getHeight() - 3.f); break;
            case Mixer: for (int i = 0; i < 3; ++i) { const float x = r.getX() + 2.f + (float) i * 5.f; g.fillRect(x - 0.75f, r.getY(), 1.5f, r.getHeight());
                    const float ky = r.getY() + 3.f + (i == 1 ? 5.f : i == 2 ? 2.f : 0.f); g.fillRoundedRectangle(x - 2.5f, ky, 5.f, 3.f, 1.f); } break;
            case Editor: g.fillRoundedRectangle(r.getX(), r.getY() + 1.f, 7.f, 3.f, 1.2f); g.fillRoundedRectangle(r.getX() + 4.f, r.getY() + 5.5f, 9.f, 3.f, 1.2f); g.fillRoundedRectangle(r.getX() + 1.f, r.getY() + 10.f, 6.f, 3.f, 1.2f); break;       
            case Tuner: { p.startNewSubPath(cx - 3.5f, r.getY()); p.lineTo(cx - 3.5f, cy + 1.5f); p.quadraticTo(cx - 3.5f, cy + 5.f, cx, cy + 5.f); p.quadraticTo(cx + 3.5f, cy + 5.f, cx + 3.5f, cy + 1.5f); p.lineTo(cx + 3.5f, r.getY());
                g.strokePath(p, PathStrokeType(1.6f)); g.drawLine(cx, cy + 5.f, cx, r.getBottom(), 1.6f); break; }                                                                                                              
        }
    }
};
struct Lcd : Component {                                      
    Project& p; std::function<float()> cpu;
    std::function<void(double)> onTempo; std::function<void()> onTempoEdit, onTap, onSig, onGrid, onInOut, onMenu;
    Rectangle<int> rPos, rTempo, rSig, rIO, rMet, rChev; double dragBpm = 0; bool dragging = false;
    explicit Lcd(Project& pr) : p(pr) {}
    void resized() override { auto r = getLocalBounds(); rChev = r.removeFromRight(26); rMet = r.removeFromRight(88); rIO = r.removeFromRight(84); rSig = r.removeFromRight(72); rTempo = r.removeFromRight(118); rPos = r; }
    static Font mono(float s, bool bold = false) { return Font(FontOptions(Font::getDefaultMonospacedFontName(), s, bold ? Font::bold : Font::plain)); }
    static void sep(Graphics& g, const Rectangle<int>& r) { g.setColour(Colour(0xff35363b)); g.drawVerticalLine(r.getX(), (float) r.getY() + 6.f, (float) r.getBottom() - 6.f); }
    void drawDigits(Graphics& g, const String& s, float x, float y, float size, Colour on, Colour dim) {   
        g.setFont(mono(size)); const float adv = size * 0.6f; bool lead = true;
        for (int i = 0; i < s.length(); ++i) { const juce_wchar c = s[i]; if (c != '0') lead = false; if (i == s.length() - 1) lead = false;
            g.setColour(lead && c == '0' ? dim : on); g.drawText(String::charToString(c), Rectangle<float>(x + (float) i * adv, y, adv, size * 1.3f), Justification::centred, false); }
    }
    void paint(Graphics& g) override {
        auto b = getLocalBounds().toFloat(); const Colour txt(0xfff2f2f5), dim(0xff4b4c53), lab(0xff9d9ea6), hot(0xffffd60a); const int H = getHeight(), half = H / 2;
        g.setColour(Colour(0xff1c1d21)); g.fillRoundedRectangle(b, 7.f); g.setColour(Colour(0xff0b0b0d)); g.drawRoundedRectangle(b.reduced(0.5f), 7.f, 1.f);
        for (auto* r : { &rTempo, &rSig, &rIO, &rMet, &rChev }) sep(g, *r);
        const double bp = jmax(1.0, p.bpm.load()), beat = p.beat.load(); const int bpb = jmax(1, p.beatsPerBar.load());
        { double sec = beat * 60.0 / bp; const bool neg = sec < 0; sec = std::abs(sec); const int h = (int) (sec / 3600.0), m = (int) std::fmod(sec / 60.0, 60.0), s = (int) std::fmod(sec, 60.0);
          const double fr = (sec - std::floor(sec)) * 30.0; const int f = (int) fr, sub = (int) ((fr - f) * 100.0);
          g.setColour(txt); g.setFont(mono(19.f, true)); g.drawText((neg ? "-" : "") + String::formatted("%02d:%02d:%02d:%02d.%02d", h, m, s, f, sub), rPos.withHeight(half + 4).translated(0, 2), Justification::centred, false);
          int bar, bt, tk; core::barBeat(beat, bpb, bar, bt, tk); const double q = beat * 4.0; const int ticks = (int) ((q - std::floor(q)) * 240.0) + 1;
          const float size = 15.f, adv = size * 0.6f; const String bs = String::formatted("%04d", bar), ts = String::formatted("%03d", ticks); const float tot = adv * (float) (bs.length() + 1 + 1 + 1 + 1 + 1 + ts.length());
          float x = (float) rPos.getCentreX() - tot * 0.5f; const float y = (float) half + 1.f;
          drawDigits(g, bs, x, y, size, txt, dim); x += adv * (float) (bs.length() + 1); drawDigits(g, String(bt), x, y, size, txt, dim); x += adv * 2.f;
          drawDigits(g, String(tk), x, y, size, txt, dim); x += adv * 2.f; drawDigits(g, ts, x, y, size, txt, dim); }
        g.setColour(txt); g.setFont(mono(17.f, true)); g.drawText(String(bp, 4), rTempo.withHeight(half + 2).translated(0, 3), Justification::centred, false);
        g.setColour(lab); g.setFont(Font(FontOptions(12.f))); g.drawText("Tap Tempo", rTempo.withTrimmedTop(half + 1), Justification::centred, false);
        g.setColour(txt); g.setFont(mono(17.f, true)); g.drawText(String(bpb) + "/4", rSig.withHeight(half + 2).translated(0, 3), Justification::centred, false);
        static const char* grids[5] = { "Off", "/4", "/8", "/16", "/32" };
        g.setColour(lab); g.setFont(Font(FontOptions(12.f))); g.drawText(grids[jlimit(0, 4, p.noteSnap)], rSig.withTrimmedTop(half + 1), Justification::centred, false);
        { const bool cyc = p.cycle; const int a = (int) std::floor(p.loopStart.load() / bpb) + 1, o = (int) std::floor(p.loopEnd.load() / bpb) + 1;
          g.setFont(Font(FontOptions(15.f))); g.setColour(cyc ? hot : txt.withAlpha(0.85f)); g.drawText(cyc ? "In  " + String(a) : String("No In"), rIO.withHeight(half + 2).translated(0, 3), Justification::centred, false);
          g.setColour(cyc ? hot : txt.withAlpha(0.85f)); g.drawText(cyc ? "Out " + String(o) : String("No Out"), rIO.withTrimmedTop(half + 1).translated(0, 1), Justification::centred, false); }
        { const float cp = jlimit(0.f, 1.f, cpu ? cpu() : 0.f); const float hd = p.recCap.load() > 0 && p.recClip >= 0 ? jlimit(0.f, 1.f, (float) p.recPos.load() / (float) p.recCap.load()) : 0.f;
          const char* names[2] = { "CPU", "HD" }; const float vals[2] = { cp, hd };
          for (int i = 0; i < 2; ++i) { const float y = (float) H * (i == 0 ? 0.30f : 0.68f); auto lr = Rectangle<float>((float) rMet.getX() + 8.f, y - 6.f, 26.f, 12.f);
              g.setColour(Colour(0xff6f7077)); g.setFont(Font(FontOptions(9.5f, Font::bold))); g.drawText(names[i], lr, Justification::centredLeft, false);
              auto tr = Rectangle<float>(lr.getRight() + 2.f, y - 2.f, (float) rMet.getRight() - lr.getRight() - 12.f, 4.f); g.setColour(Colour(0xff2c2d32)); g.fillRoundedRectangle(tr, 2.f);
              g.setColour(vals[i] > 0.85f ? Colour(0xffff9f0a) : Colour(0xffb9bac1)); g.fillRoundedRectangle(tr.withWidth(jmax(0.f, tr.getWidth() * vals[i])), 2.f); } }
        { Path c; const auto cr = rChev.toFloat(); c.startNewSubPath(cr.getCentreX() - 4.f, cr.getCentreY() - 2.f); c.lineTo(cr.getCentreX(), cr.getCentreY() + 2.f); c.lineTo(cr.getCentreX() + 4.f, cr.getCentreY() - 2.f);
          g.setColour(lab); g.strokePath(c, PathStrokeType(1.6f)); }
    }
    void mouseDown(const MouseEvent& e) override {
        const auto pt = e.getPosition(); const bool top = pt.y < getHeight() / 2; dragging = false;
        if (rTempo.contains(pt)) { if (top) { dragBpm = p.bpm; dragging = true; } else if (onTap) onTap(); }
        else if (rSig.contains(pt)) { if (top) { if (onSig) onSig(); } else if (onGrid) onGrid(); }
        else if (rIO.contains(pt)) { if (onInOut) onInOut(); }
        else if (rChev.contains(pt) || rMet.contains(pt)) { if (onMenu) onMenu(); }
    }
    void mouseDrag(const MouseEvent& e) override { if (dragging && onTempo) onTempo(dragBpm - (double) e.getDistanceFromDragStartY() * (e.mods.isShiftDown() ? 0.01 : 0.25)); }
    void mouseUp(const MouseEvent&) override { dragging = false; }
    void mouseDoubleClick(const MouseEvent& e) override { if (rTempo.contains(e.getPosition()) && e.y < getHeight() / 2 && onTempoEdit) onTempoEdit(); }
};
struct TunerPanel : Component, Timer {                                      
    static constexpr int kH = 132, N = 1 << 14;
    Project& p; std::function<void()> onClose;
    std::atomic<bool> active { false }; std::atomic<int> inCh { 0 }, numIn { 2 }, wr { 0 }; std::atomic<float> peak { 0 };
    std::vector<float> ring = std::vector<float>((size_t) N, 0.f), work;
    double a4 = 440.0, lastGood = 0; float hz = 0.f, cents = 0.f, level = 0.f; int midi = -1; bool hasSignal = false;
    Rectangle<int> rGauge, rNote, rRef, rMinus, rPlus, rIn, rClose, rMeter;
    explicit TunerPanel(Project& pr) : p(pr) {}
    void push(const float* x, int n) {                                        
        int w = wr.load(std::memory_order_relaxed); float pk = 0.f;
        for (int i = 0; i < n; ++i) { ring[(size_t) w] = x[i]; w = (w + 1) & (N - 1); pk = jmax(pk, std::abs(x[i])); }
        wr.store(w, std::memory_order_release); peak.store(jmax(pk, peak.load(std::memory_order_relaxed)), std::memory_order_relaxed); }
    void visibilityChanged() override { active = isVisible(); if (isVisible()) startTimerHz(25); else { stopTimer(); hasSignal = false; midi = -1; } }
    void timerCallback() override {
        const double sr = jmax(8000.0, p.sr); const int n = 3072, dec = jlimit(1, N / n, (int) (sr / 22000.0)), need = n * dec;      
        work.resize((size_t) n); const int end = wr.load(std::memory_order_acquire), start = (end - need + N) & (N - 1);
        for (int i = 0; i < n; ++i) { float s = 0.f; for (int k = 0; k < dec; ++k) s += ring[(size_t) ((start + i * dec + k) & (N - 1))]; work[(size_t) i] = s / (float) dec; }
        const float f = core::detectPitch(work.data(), n, sr / (double) dec, 28.f, 1400.f); const double now = Time::getMillisecondCounterHiRes();
        level = jmax(peak.exchange(0.f), level * 0.85f);
        if (f > 0.f) { int m0; double c0; core::nearestNote((double) f, a4, m0, c0);
            if (! hasSignal || m0 != midi) hz = f; else hz += (f - hz) * 0.35f;                                           
            double cd; core::nearestNote((double) hz, a4, midi, cd); cents = (float) cd; hasSignal = true; lastGood = now; }
        else if (hasSignal && now - lastGood > 700.0) hasSignal = false;                                                    
        repaint(); }
    void resized() override {
        auto r = getLocalBounds().reduced(14, 10); rClose = { getWidth() - 32, 6, 22, 22 };
        rNote = r.removeFromLeft(190); r.removeFromLeft(14); auto right = r.removeFromRight(190); r.removeFromRight(14); rGauge = r; right.removeFromTop(20);
        rRef = right.removeFromTop(34); right.removeFromTop(8); rIn = right.removeFromTop(24); right.removeFromTop(10); rMeter = right.removeFromTop(10);
        rMinus = { rRef.getRight() - 62, rRef.getY() + 5, 28, 24 }; rPlus = { rRef.getRight() - 30, rRef.getY() + 5, 28, 24 }; }
    void mouseDown(const MouseEvent& e) override {
        const auto pt = e.getPosition();
        if (rClose.contains(pt)) { if (onClose) onClose(); } else if (rMinus.contains(pt)) a4 = jmax(415.0, a4 - 1.0); else if (rPlus.contains(pt)) a4 = jmin(466.0, a4 + 1.0);
        else if (rIn.contains(pt)) inCh = (inCh.load() + 1) % jmax(1, numIn.load()); repaint(); }
    void mouseDoubleClick(const MouseEvent& e) override { const auto pt = e.getPosition(); if (rRef.contains(pt) && ! rMinus.contains(pt) && ! rPlus.contains(pt)) { a4 = 440.0; repaint(); } }
    static float textW(const Font& f, const String& s) { GlyphArrangement ga; ga.addLineOfText(f, s, 0.f, 0.f); return ga.getBoundingBox(0, -1, true).getWidth(); }
    void paint(Graphics& g) override {
        const auto b = getLocalBounds().toFloat(); g.setGradientFill(ColourGradient(Colour(0xff2f3035), 0.f, 0.f, Colour(0xff222327), 0.f, b.getHeight(), false)); g.fillAll();
        g.setColour(Colour(0xff0f1012)); g.fillRect(0, getHeight() - 1, getWidth(), 1);
        const bool ok = hasSignal; const float ac = std::abs(cents); const Colour good(0xff32d74b), warn(0xffffd60a), bad(0xffff6b57), idle(0xff6b6c73);
        const Colour state = ! ok ? idle : ac <= 5.f ? good : ac <= 20.f ? warn : bad; const Colour lab(0xff9a9ba3), txt(0xfff2f2f5);
        g.setColour(lab); g.setFont(Font(FontOptions(10.5f, Font::bold))); g.drawText("TUNER", rNote.getX(), 6, 80, 14, Justification::centredLeft, false);
        { const String name = ok ? String(core::noteName(midi)) : String("--"), oct = ok ? String(core::noteOctave(midi)) : String(); const Font big(FontOptions(60.f, Font::bold)), small(FontOptions(24.f, Font::bold));
          const float w1 = textW(big, name), w2 = oct.isEmpty() ? 0.f : textW(small, oct) + 3.f; const float x = (float) rNote.getCentreX() - (w1 + w2) * 0.5f, y = (float) rNote.getY() + 6.f;
          g.setColour(ok ? state : idle); g.setFont(big); g.drawText(name, Rectangle<float>(x, y, w1 + 4.f, 62.f), Justification::centredLeft, false);
          if (! oct.isEmpty()) { g.setFont(small); g.drawText(oct, Rectangle<float>(x + w1 + 3.f, y + 30.f, w2 + 4.f, 28.f), Justification::centredLeft, false); }
          g.setColour(ok ? txt : lab); g.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 14.f, Font::plain)));
          g.drawText(ok ? String(hz, 1) + " Hz" : String("Play a note"), rNote.withTrimmedTop(76).withHeight(20), Justification::centred, false); }
        { const auto r = rGauge; g.setFont(Font(FontOptions(15.f, Font::bold))); g.setColour(state);
          const String msg = ! ok ? String("Listening...") : ac <= 5.f ? String("In tune") : (cents > 0 ? String("Sharp  +") : String("Flat  -")) + String((int) std::lround(ac)) + " cents";
          g.drawText(msg, r.getX(), r.getY() + 2, r.getWidth(), 22, Justification::centred, false);
          const float gx = (float) r.getX() + 12.f, gw = (float) r.getWidth() - 24.f, ty = (float) r.getY() + 40.f, th = 40.f; auto xc = [gx, gw](float c) { return gx + (c + 50.f) / 100.f * gw; };
          g.setColour(Colour(0xff17181a)); g.fillRoundedRectangle(gx - 6.f, ty - 6.f, gw + 12.f, th + 12.f, 8.f);
          g.setColour(good.withAlpha(ok && ac <= 5.f ? 0.35f : 0.14f)); g.fillRect(xc(-5.f), ty - 6.f, xc(5.f) - xc(-5.f), th + 12.f);
          for (int c = -50; c <= 50; c += 5) { const bool major = c % 25 == 0, mid = c % 10 == 0; const float h = major ? th : mid ? th * 0.62f : th * 0.38f; g.setColour(c == 0 ? good : Colour(major ? 0xff9a9ba3 : 0xff55565d)); g.fillRect(xc((float) c) - (major ? 1.f : 0.5f), ty + (th - h) * 0.5f, major ? 2.f : 1.f, h); }
          g.setFont(Font(FontOptions(10.5f))); g.setColour(lab); for (int c : { -50, -25, 0, 25, 50 }) g.drawText(c == 0 ? String("0") : (c > 0 ? "+" : "") + String(c), (int) xc((float) c) - 20, (int) (ty + th + 10.f), 40, 14, Justification::centred, false);
          if (ok) { const float nx = xc(jlimit(-50.f, 50.f, cents)); g.setColour(state.withAlpha(0.22f)); g.fillRoundedRectangle(nx - 5.f, ty - 6.f, 10.f, th + 12.f, 4.f);
              g.setColour(state); g.fillRoundedRectangle(nx - 1.5f, ty - 6.f, 3.f, th + 12.f, 1.5f); Path tri; tri.addTriangle(nx - 6.f, ty - 14.f, nx + 6.f, ty - 14.f, nx, ty - 6.f); g.fillPath(tri); } }
        { auto drawBtn = [&g](Rectangle<int> r, const String& t) { g.setColour(Colour(0xff3a3b41)); g.fillRoundedRectangle(r.toFloat(), 5.f); g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.f, 1.f);
              g.setColour(Colour(0xfff0f0f4)); g.setFont(Font(FontOptions(15.f, Font::bold))); g.drawText(t, r, Justification::centred, false); };
          g.setColour(lab); g.setFont(Font(FontOptions(10.f, Font::bold))); g.drawText("REFERENCE  A4", rRef.getX(), rRef.getY(), 120, 14, Justification::centredLeft, false);
          g.setColour(txt); g.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 16.f, Font::bold))); g.drawText(String(a4, 0) + " Hz", rRef.getX(), rRef.getY() + 14, 90, 20, Justification::centredLeft, false);
          drawBtn(rMinus, String::charToString((juce_wchar) 0x2212)); drawBtn(rPlus, "+");
          const auto f = rIn.toFloat(); g.setColour(Colour(0xff3a3b41)); g.fillRoundedRectangle(f, 5.f); g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(f.reduced(0.5f), 5.f, 1.f);
          g.setColour(lab); g.setFont(Font(FontOptions(11.f))); g.drawText("Input", rIn.withTrimmedLeft(8).withWidth(44), Justification::centredLeft, false);
          g.setColour(txt); g.setFont(Font(FontOptions(12.f, Font::bold))); g.drawText("Channel " + String(inCh.load() + 1), rIn.withTrimmedLeft(52), Justification::centredLeft, false);
          const auto m = rMeter.toFloat(); g.setColour(Colour(0xff17181a)); g.fillRoundedRectangle(m, 3.f); const float lv = core::meterPos(level);
          g.setColour(lv > 0.9f ? bad : lv > 0.75f ? warn : good); g.fillRoundedRectangle(m.withWidth(jmax(0.f, m.getWidth() * lv)), 3.f);
          g.setColour(Colour(0xff55565d)); g.setFont(Font(FontOptions(10.f, Font::bold))); g.drawText("Input level", rMeter.translated(0, 11).withHeight(12), Justification::centredLeft, false); }
        { g.setColour(Colour(0xff9a9ba3)); const auto c = rClose.toFloat().reduced(7.f); g.drawLine(c.getX(), c.getY(), c.getRight(), c.getBottom(), 1.6f); g.drawLine(c.getRight(), c.getY(), c.getX(), c.getBottom(), 1.6f); }
    }
};
static float textWidth(const Font& f, const String& s) { GlyphArrangement ga; ga.addLineOfText(f, s, 0.f, 0.f); return ga.getBoundingBox(0, -1, true).getWidth(); }
struct PickItem { int id = 0; String name, sub, badge; bool enabled = true, ticked = false; int chip = 0; };            
struct ChooserList : Component, ListBoxModel {
    String title, subtitle, emptyText { "Nothing here yet" }; std::vector<PickItem> items; StringArray chips; int chipSel = 0; float progress = -1.f; bool singleClick = false;
    std::function<void(int)> onPick, onAction; std::function<void(int, Point<int>)> onContext;
    TextEditor search; ListBox list; OwnedArray<TextButton> actionBtns; std::vector<int> actionIds, rows; Rectangle<int> searchR, chipsR, titleR, clearR; std::vector<Rectangle<int>> chipR;
    struct Keys : KeyListener {                                                
        ChooserList& o; explicit Keys(ChooserList& x) : o(x) {}
        bool keyPressed(const KeyPress& k, Component*) override {
            const int n = (int) o.rows.size(); if (k == KeyPress::escapeKey && o.search.getText().isNotEmpty()) { o.search.clear(); return true; } if (n == 0) return false;
            int r = o.list.getSelectedRow(); if (k.isKeyCode(KeyPress::downKey)) r = jmin(n - 1, r + 1); else if (k.isKeyCode(KeyPress::upKey)) r = jmax(0, r - 1); else return false;
            o.list.selectRow(r); return true; } } keys { *this };
    ChooserList() {
        search.setTextToShowWhenEmpty("Search", Colour(0xff7d7e85)); search.setIndents(34, 0); search.setJustification(Justification::centredLeft); search.setFont(Font(FontOptions(15.f)));
        search.onTextChange = [this] { rebuild(); }; search.onReturnKey = [this] { pickRow(list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0); }; search.addKeyListener(&keys);
        list.setModel(this); list.setRowHeight(48); list.setOutlineThickness(0); list.setColour(ListBox::backgroundColourId, Colours::transparentBlack);
        addAndMakeVisible(search); addAndMakeVisible(list); }
    ~ChooserList() override { search.removeKeyListener(&keys); }
    void setActions(std::vector<std::pair<String, int>> a) {
        actionBtns.clear(); actionIds.clear();
        for (auto& pr : a) { auto* b = actionBtns.add(new TextButton(pr.first)); const int id = pr.second; actionIds.push_back(id); b->onClick = [this, id] { auto cb = onAction; dismissParent(); if (cb) cb(id); }; addAndMakeVisible(b); }
        resized(); }
    TextButton* actionButton(int id) { for (size_t i = 0; i < actionIds.size(); ++i) if (actionIds[i] == id) return actionBtns[(int) i]; return nullptr; }
    void dismissParent() { if (auto* cb = findParentComponentOfClass<CallOutBox>()) cb->dismiss(); }
    void rebuild() {
        rows.clear(); StringArray tok; tok.addTokens(search.getText().trim(), " ", ""); tok.removeEmptyStrings();
        for (int i = 0; i < (int) items.size(); ++i) { const auto& it = items[(size_t) i]; if (chipSel > 0 && it.chip != chipSel) continue; bool ok = true;
            for (auto& t : tok) if (! (it.name.containsIgnoreCase(t) || it.sub.containsIgnoreCase(t) || it.badge.containsIgnoreCase(t))) { ok = false; break; } if (ok) rows.push_back(i); }
        list.updateContent(); list.deselectAllRows(); if (! rows.empty()) list.selectRow(0); list.repaint(); repaint(); }
    void pickRow(int row) {
        if (row < 0 || row >= (int) rows.size()) return; const auto& it = items[(size_t) rows[(size_t) row]]; if (! it.enabled) return;
        const int id = it.id; auto cb = onPick; dismissParent(); if (cb) cb(id); }
    void visibilityChanged() override { if (isVisible()) Timer::callAfterDelay(60, [safe = Component::SafePointer<ChooserList>(this)] { if (safe != nullptr) safe->search.grabKeyboardFocus(); }); }
    void resized() override {
        auto r = getLocalBounds().reduced(14, 12); titleR = {}; if (title.isNotEmpty()) { titleR = r.removeFromTop(30); r.removeFromTop(8); }
        searchR = r.removeFromTop(36); search.setBounds(searchR); clearR = { searchR.getRight() - 30, searchR.getY() + 6, 24, 24 }; r.removeFromTop(10);
        chipR.clear(); chipsR = {}; if (chips.size() > 1) { chipsR = r.removeFromTop(28); r.removeFromTop(8); int x = chipsR.getX(); for (auto& c : chips) { const int w = 26 + (int) ((float) c.length() * 7.5f); chipR.push_back({ x, chipsR.getY(), w, 28 }); x += w + 6; } }
        if (! actionBtns.isEmpty()) { auto f = r.removeFromBottom(38); r.removeFromBottom(10); int x = f.getX(); for (auto* b : actionBtns) { const int w = jmax(110, b->getButtonText().length() * 8 + 34); b->setBounds(x, f.getY(), w, f.getHeight()); x += w + 8; } }
        list.setBounds(r); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff25262b));
        if (! titleR.isEmpty()) { g.setColour(Colour(0xfff2f2f5)); g.setFont(Font(FontOptions(18.f, Font::bold))); g.drawText(title, titleR.withTrimmedRight(230), Justification::centredLeft, true);
            g.setColour(Colour(0xff8a8b93)); g.setFont(Font(FontOptions(12.5f))); g.drawText(subtitle, titleR.withLeft(titleR.getRight() - 300), Justification::centredRight, true);
            if (progress >= 0.f) { g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle((float) titleR.getX(), (float) titleR.getBottom() + 1.f, (float) titleR.getWidth(), 3.f, 1.5f); g.setColour(Colour(0xfff2f2f5)); g.fillRoundedRectangle((float) titleR.getX(), (float) titleR.getBottom() + 1.f, (float) titleR.getWidth() * jlimit(0.02f, 1.f, progress), 3.f, 1.5f); } }
        for (size_t i = 0; i < chipR.size(); ++i) { const bool s = (int) i == chipSel; g.setColour(s ? Colour(0xfff2f2f5) : Colour(0xff34353a)); g.fillRoundedRectangle(chipR[i].toFloat(), 14.f);
            g.setColour(s ? Colour(0xff17181b) : Colour(0xffc9cad2)); g.setFont(Font(FontOptions(12.5f, Font::bold))); g.drawText(chips[(int) i], chipR[i], Justification::centred, false); }
        if (rows.empty()) { const auto lr = list.getBounds(); g.setColour(Colour(0xff8f9097)); g.setFont(Font(FontOptions(15.f, Font::bold)));
            g.drawText(search.getText().isNotEmpty() ? "No matches for \"" + search.getText().trim() + "\"" : emptyText, lr.reduced(20, 0).translated(0, -10), Justification::centred, true); } }
    void paintOverChildren(Graphics& g) override {                              
        const float cx = (float) searchR.getX() + 17.f, cy = (float) searchR.getCentreY() - 1.f; g.setColour(Colour(0xff8f9097)); g.drawEllipse(cx - 5.f, cy - 5.f, 10.f, 10.f, 1.6f); g.drawLine(cx + 3.5f, cy + 3.5f, cx + 8.f, cy + 8.f, 1.8f);
        if (search.getText().isNotEmpty()) { const auto c = clearR.toFloat().reduced(7.f); g.drawLine(c.getX(), c.getY(), c.getRight(), c.getBottom(), 1.6f); g.drawLine(c.getRight(), c.getY(), c.getX(), c.getBottom(), 1.6f); } }
    void mouseDown(const MouseEvent& e) override {
        const auto pt = e.getPosition(); if (search.getText().isNotEmpty() && clearR.contains(pt)) { search.clear(); search.grabKeyboardFocus(); return; }
        for (size_t i = 0; i < chipR.size(); ++i) if (chipR[i].contains(pt)) { chipSel = (int) i; rebuild(); return; } }
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem(int row, Graphics& g, int w, int h, bool sel) override {
        if (row < 0 || row >= (int) rows.size()) return; const auto& it = items[(size_t) rows[(size_t) row]]; const float al = it.enabled ? 1.f : 0.45f;
        if (sel) { g.setColour(Colour(0x26ffffff)); g.fillRoundedRectangle(2.f, 2.f, (float) w - 4.f, (float) h - 4.f, 10.f); }
        const auto tile = Rectangle<float>(10.f, ((float) h - 32.f) * 0.5f, 32.f, 32.f); g.setColour(Colour(0xff3a3b41)); g.fillRoundedRectangle(tile, 9.f);
        g.setColour(Colour(0xffd0d1d8).withMultipliedAlpha(al)); g.setFont(Font(FontOptions(15.f, Font::bold))); g.drawText(it.name.substring(0, 1).toUpperCase(), tile, Justification::centred, false);
        float bw = 0.f; if (it.badge.isNotEmpty()) { const Font bf(FontOptions(10.5f, Font::bold)); bw = textWidth(bf, it.badge) + 16.f; const auto br = Rectangle<float>((float) w - bw - 12.f, ((float) h - 20.f) * 0.5f, bw, 20.f);
            g.setColour(Colour(0xff4a4b52)); g.drawRoundedRectangle(br, 10.f, 1.f); g.setColour(Colour(0xffa0a1a9).withMultipliedAlpha(al)); g.setFont(bf); g.drawText(it.badge, br, Justification::centred, false); }
        if (it.ticked) { Path t; const float cx = (float) w - bw - 28.f, cy = (float) h * 0.5f; t.startNewSubPath(cx - 4.f, cy); t.lineTo(cx - 1.f, cy + 3.f); t.lineTo(cx + 4.5f, cy - 3.5f); g.setColour(Colours::white); g.strokePath(t, PathStrokeType(1.8f, PathStrokeType::curved, PathStrokeType::rounded)); }
        const int tx = 54, tw2 = jmax(40, w - tx - (int) bw - 20 - (it.ticked ? 22 : 0));
        g.setColour(Colour(0xfff0f0f4).withMultipliedAlpha(al)); g.setFont(Font(FontOptions(14.5f, Font::bold))); g.drawText(it.name, tx, 7, tw2, 20, Justification::centredLeft, true);
        if (it.sub.isNotEmpty()) { g.setColour(Colour(0xff8a8b93).withMultipliedAlpha(al)); g.setFont(Font(FontOptions(12.f))); g.drawText(it.sub, tx, 27, tw2, 16, Justification::centredLeft, true); } }
    void listBoxItemClicked(int row, const MouseEvent& e) override {
        if (row < 0 || row >= (int) rows.size()) return;
        if (e.mods.isPopupMenu()) { list.selectRow(row); if (onContext) onContext(items[(size_t) rows[(size_t) row]].id, e.getScreenPosition()); } else if (singleClick) pickRow(row); }
    void listBoxItemDoubleClicked(int row, const MouseEvent&) override { pickRow(row); }
    void returnKeyPressed(int row) override { pickRow(row); }
};
struct PluginManagerPanel : Component, Thread, ChangeListener, Timer {                    
    AudioPluginFormatManager& fm; KnownPluginList& known; File dead; ChooserList cl; std::atomic<float> frac { -1.f }; std::atomic<bool> scanning { false }; CriticalSection cs; String current; std::vector<PluginDescription> descs;
    PluginManagerPanel(AudioPluginFormatManager& f, KnownPluginList& k, const File& d) : Thread("Plug-in scan"), fm(f), known(k), dead(d) {
        cl.title = "Plug-ins"; cl.chips = { "All", "Instruments", "Effects" }; cl.emptyText = "No plug-ins yet. Press Scan to find your VST3 / AU plug-ins."; cl.setActions({ { "Scan for Plug-ins", 1 }, { "Clear List", 2 } });
        cl.onAction = [this](int id) { if (id == 1) startScan(); else if (id == 2) { known.clear(); known.clearBlacklistedFiles(); } };
        cl.onContext = [this](int idx, Point<int> pos) {
            if (idx < 0 || idx >= (int) descs.size()) return; const auto dsc = descs[(size_t) idx]; const bool real = File::isAbsolutePath(dsc.fileOrIdentifier) && File(dsc.fileOrIdentifier).exists(); PopupMenu m; m.addItem(1, "Show in File Manager", real); m.addItem(2, "Remove from List");
            m.showMenuAsync(PopupMenu::Options().withTargetScreenArea(Rectangle<int>(pos.x, pos.y, 1, 1)), [this, dsc, real](int r) { if (r == 1 && real) File(dsc.fileOrIdentifier).revealToUser(); else if (r == 2) known.removeType(dsc); }); };
        addAndMakeVisible(cl); known.addChangeListener(this); refreshList(); }
    ~PluginManagerPanel() override { known.removeChangeListener(this); stopThread(8000); }
    void resized() override { cl.setBounds(getLocalBounds()); }
    void changeListenerCallback(ChangeBroadcaster*) override { refreshList(); }
    void refreshList() {
        descs.clear(); for (auto& d : known.getTypes()) descs.push_back(d); std::sort(descs.begin(), descs.end(), [](const PluginDescription& a, const PluginDescription& b) { return a.name.compareIgnoreCase(b.name) < 0; });
        cl.items.clear(); for (size_t i = 0; i < descs.size(); ++i) { const auto& d = descs[i]; PickItem it; it.id = (int) i; it.name = d.name; it.sub = d.manufacturerName + (d.category.isNotEmpty() ? " - " + d.category : String()); it.badge = d.pluginFormatName; it.chip = d.isInstrument ? 1 : 2; cl.items.push_back(it); }
        if (! scanning) cl.subtitle = String((int) descs.size()) + (descs.size() == 1 ? " plug-in" : " plug-ins"); cl.rebuild(); }
    void startScan() {
        if (isThreadRunning() || scanning) return; scanning = true; frac = 0.f; if (auto* b = cl.actionButton(1)) { b->setEnabled(false); b->setButtonText("Scanning..."); }
        startThread(); startTimerHz(10); }
    void run() override {
        const int n = jmax(1, fm.getNumFormats());
        for (int i = 0; i < fm.getNumFormats() && ! threadShouldExit(); ++i) {
            auto* fmt = fm.getFormat(i);
            auto ids = fmt->searchPathsForPlugins(fmt->getDefaultLocationsToSearch(), true, false);
            for (int k = 0; k < ids.size() && ! threadShouldExit(); ++k) {
                const auto id = ids[k];
                frac = ((float) i + (float) k / (float) jmax(1, ids.size())) / (float) n;
                { const ScopedLock sl(cs); current = fmt->getName() + ": " + fmt->getNameOfPluginFromIdentifier(id); }
                if (known.getBlacklistedFiles().contains(id) || known.isListingUpToDate(id, *fmt)) continue;
                // each plug-in is scanned in a separate process, so a crashing plug-in cannot take the app down
                auto out = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("minidaw_scan", ".xml");
                ChildProcess cp;
                bool ok = cp.start(StringArray { File::getSpecialLocation(File::currentExecutableFile).getFullPathName(),
                                                 "--scan-plugin", fmt->getName(), id, out.getFullPathName() });
                if (ok) { ok = cp.waitForProcessToFinish(30000) && cp.getExitCode() == 0; if (! ok) cp.kill(); }
                if (ok) { if (auto xml = parseXML(out)) for (auto* e : xml->getChildIterator()) { PluginDescription d; if (d.loadFromXml(*e)) known.addType(d); } }
                else known.addToBlacklist(id);
                out.deleteFile(); } }
        MessageManager::callAsync([safe = Component::SafePointer<PluginManagerPanel>(this)] { if (safe != nullptr) safe->scanFinished(); }); }
    void timerCallback() override { const ScopedLock sl(cs); cl.progress = frac; cl.subtitle = current.isEmpty() ? String("Scanning...") : "Scanning " + current; cl.repaint(); }
    void scanFinished() {
        stopTimer(); scanning = false; frac = -1.f; cl.progress = -1.f; { const ScopedLock sl(cs); current = String(); }
        if (auto* b = cl.actionButton(1)) { b->setEnabled(true); b->setButtonText("Scan for Plug-ins"); } refreshList(); }
};
struct StartScreen : Component, ListBoxModel {
    enum Tab { NewTab, RecentTab };
    static constexpr int kTemplates = 6, sideW = 236; static constexpr int kSig[8] = { 2, 3, 4, 5, 6, 7, 9, 12 };
    struct Tpl { const char* name; const char* sub; uint32 c1, c2; };
    static const Tpl& tpl(int i) {
        static const Tpl t[kTemplates] = {
            { "Empty Project", "Start from nothing and add your own tracks", 0xff44464d, 0xff2d2e33 },
            { "Instrument", "One synth track, ready to play and write", 0xff44464d, 0xff2d2e33 },
            { "Recording", "One armed audio track for vocals, guitar or any input", 0xff44464d, 0xff2d2e33 },
            { "Beat Maker", "Drum kit and sub bass for building grooves", 0xff44464d, 0xff2d2e33 },
            { "Band", "Keys, bass, drums and an audio track", 0xff44464d, 0xff2d2e33 },
            { "Electronic", "Lead, pad, sub bass and drums", 0xff44464d, 0xff2d2e33 } };
        return t[jlimit(0, kTemplates - 1, i)]; }
    StringArray* recent = nullptr; std::vector<File> shown; File dir; Colour accent { 0xff5b8def };
    std::function<void(const File&)> onOpenFile, onRemoveRecent, onDirChanged; std::function<void()> onOpenDialog, onOpenBackup, onSettings, onAudio, onPlugins, onClearRecent;
    std::function<void(int, const String&, const File&, double, int)> onCreate;                 
    Tab tab = NewTab; int tplSel = 1, hoverCard = -1, hoverNav = -1, formY = 0, sigLblX = 0; std::unique_ptr<FileChooser> chooser;
    TextEditor nameEd, search; Slider tempoSl { Slider::LinearHorizontal, Slider::TextBoxRight }; ComboBox sigBox;
    TextButton createB { "Create" }, folderB { "Choose..." }, openB { "Open" }, clearB { "Clear List" }; ListBox list;
    Rectangle<int> cardR[kTemplates], navR[7], pathR;
    StartScreen() {
        setOpaque(true); setWantsKeyboardFocus(true); getProperties().set("mono", true);
        for (auto* e : { &nameEd, &search }) { e->setColour(TextEditor::backgroundColourId, Colour(0xff15161a)); e->setColour(TextEditor::outlineColourId, Colour(0xff3a3b40)); e->setFont(Font(FontOptions(15.f))); }
        nameEd.setText("My Project", false); nameEd.setSelectAllWhenFocused(true); nameEd.onReturnKey = [this] { create(); };
        search.setTextToShowWhenEmpty("Search recent projects", Colour(0xff7d7e85)); search.onTextChange = [this] { refresh(); };
        tempoSl.setRange(40, 240, 1); tempoSl.setValue(120, dontSendNotification); tempoSl.setDoubleClickReturnValue(true, 120.0); tempoSl.setTextValueSuffix(" BPM");
        for (int i = 0; i < 8; ++i) sigBox.addItem(String(kSig[i]) + " / 4", i + 1); sigBox.setSelectedId(3, dontSendNotification);
        createB.setColour(TextButton::buttonColourId, Colour(0xfff2f2f5)); createB.setColour(TextButton::textColourOffId, Colour(0xff17181b)); createB.onClick = [this] { create(); }; folderB.onClick = [this] { chooseFolder(); };
        openB.onClick = [this] { openRow(list.getSelectedRow()); }; clearB.onClick = [this] { if (onClearRecent) onClearRecent(); };
        list.setModel(this); list.setRowHeight(62); list.setOutlineThickness(0); list.setColour(ListBox::backgroundColourId, Colours::transparentBlack);
        for (Component* c : std::initializer_list<Component*> { &nameEd, &search, &tempoSl, &sigBox, &createB, &folderB, &openB, &clearB, &list }) addAndMakeVisible(c);
        setTab(NewTab); }
    void setAccent(Colour) {}                                                   
    void resetForm() { nameEd.setText("My Project", false); tempoSl.setValue(120, dontSendNotification); sigBox.setSelectedId(3, dontSendNotification); search.clear(); tplSel = 1; }
    void setTab(Tab t) {
        tab = t; const bool n = t == NewTab;
        for (Component* c : std::initializer_list<Component*> { &nameEd, &tempoSl, &sigBox, &createB, &folderB }) c->setVisible(n);
        for (Component* c : std::initializer_list<Component*> { &search, &list, &openB, &clearB }) c->setVisible(! n);
        refresh(); resized(); repaint(); }
    void refresh() {
        shown.clear(); const String q = search.getText().trim();
        if (recent != nullptr) for (auto& s : *recent) { const File f(s); if (q.isEmpty() || f.getFileName().containsIgnoreCase(q) || f.getParentDirectory().getFullPathName().containsIgnoreCase(q)) shown.push_back(f); }
        list.updateContent(); if (list.getSelectedRow() < 0 && ! shown.empty()) list.selectRow(0);
        openB.setEnabled(! shown.empty()); clearB.setEnabled(recent != nullptr && recent->size() > 0); list.repaint(); repaint(); }
    void create() { String n = File::createLegalFileName(nameEd.getText().trim()); if (n.isEmpty()) n = "Untitled"; if (onCreate) onCreate(tplSel, n, dir, tempoSl.getValue(), kSig[jlimit(0, 7, sigBox.getSelectedId() - 1)]); }
    void chooseFolder() {
        chooser = std::make_unique<FileChooser>("Choose where new projects are saved", dir, "");
        chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories, [this](const FileChooser& fc) {
            const auto f = fc.getResult(); if (f != File() && f.isDirectory()) { dir = f; if (onDirChanged) onDirChanged(f); repaint(); } }); }
    void openFile(const File& f) {
        if (! f.existsAsFile()) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Open", "Can't find \"" + f.getFileName() + "\". It may have been moved or deleted, so it was removed from the list."); if (onRemoveRecent) onRemoveRecent(f); return; }
        if (onOpenFile) onOpenFile(f); }
    void openRow(int row) { if (row >= 0 && row < (int) shown.size()) openFile(shown[(size_t) row]); }
    int getNumRows() override { return (int) shown.size(); }
    static String when(const Time& t) {
        const Time now = Time::getCurrentTime(), yest = now - RelativeTime::days(1);
        auto same = [](const Time& a, const Time& b) { return a.getYear() == b.getYear() && a.getMonth() == b.getMonth() && a.getDayOfMonth() == b.getDayOfMonth(); };
        if (same(t, now)) return "Today, " + t.toString(false, true, false, true); if (same(t, yest)) return "Yesterday, " + t.toString(false, true, false, true); return t.toString(true, false); }
    void paintListBoxItem(int row, Graphics& g, int w, int h, bool sel) override {
        if (row < 0 || row >= (int) shown.size()) return; const File& f = shown[(size_t) row]; const bool ok = f.existsAsFile();
        if (sel) { g.setColour(Colour(0x26ffffff)); g.fillRoundedRectangle(0.f, 2.f, (float) w, (float) h - 4.f, 9.f); }
        const Colour tc(0xff4f5058); const auto th = Rectangle<float>(12.f, ((float) h - 40.f) * 0.5f, 40.f, 40.f);
        g.setGradientFill(ColourGradient(tc.brighter(0.15f), th.getX(), th.getY(), tc.darker(0.55f), th.getRight(), th.getBottom(), false)); g.fillRoundedRectangle(th, 9.f);
        g.setColour(Colours::white.withAlpha(ok ? 0.95f : 0.4f)); g.setFont(Font(FontOptions(19.f, Font::bold))); g.drawText(f.getFileNameWithoutExtension().substring(0, 1).toUpperCase(), th, Justification::centred, false);
        const int tx = 66, rw = 200, tw = jmax(40, w - tx - rw - 12);
        g.setColour(ok ? Colour(0xfff0f0f4) : Colour(0xff8f9097)); g.setFont(Font(FontOptions(15.5f, Font::bold))); g.drawText(f.getFileNameWithoutExtension(), tx, 11, tw, 20, Justification::centredLeft, true);
        g.setColour(ok ? Colour(0xff8a8b93) : Colour(0xffff6b57)); g.setFont(Font(FontOptions(12.f))); g.drawText(ok ? f.getParentDirectory().getFullPathName() : String("File not found"), tx, 33, tw, 18, Justification::centredLeft, true);
        if (ok) { g.setColour(Colour(0xffc9cad2)); g.setFont(Font(FontOptions(12.5f))); g.drawText(when(f.getLastModificationTime()), w - rw - 6, 12, rw, 18, Justification::centredRight, false);
            g.setColour(Colour(0xff7d7e85)); g.drawText(File::descriptionOfSizeInBytes(f.getSize()), w - rw - 6, 33, rw, 18, Justification::centredRight, false); } }
    void listBoxItemDoubleClicked(int row, const MouseEvent&) override { openRow(row); }
    void returnKeyPressed(int row) override { openRow(row); }
    void deleteKeyPressed(int row) override { if (row >= 0 && row < (int) shown.size() && onRemoveRecent) onRemoveRecent(shown[(size_t) row]); }
    void listBoxItemClicked(int row, const MouseEvent& e) override {
        if (! e.mods.isPopupMenu() || row < 0 || row >= (int) shown.size()) return; list.selectRow(row); const File f = shown[(size_t) row];
#if JUCE_MAC
        const String fm = "Reveal in Finder";
#elif JUCE_WINDOWS
        const String fm = "Show in Explorer";
#else
        const String fm = "Show in File Manager";
#endif
        PopupMenu m; m.addItem(1, "Open"); m.addItem(2, fm, f.exists()); m.addItem(3, "Copy Path"); m.addSeparator(); m.addItem(4, "Remove from Recent");
        m.showMenuAsync(PopupMenu::Options(), [this, f](int r) { if (r == 1) openFile(f); else if (r == 2) f.revealToUser(); else if (r == 3) SystemClipboard::copyTextToClipboard(f.getFullPathName()); else if (r == 4 && onRemoveRecent) onRemoveRecent(f); }); }
    void resized() override {
        const int W = getWidth(), H = getHeight(), mx = sideW + 36, mw = jmax(200, W - mx - 36);
        { int y = 112; for (int i = 0; i < 4; ++i) { navR[i] = { 14, y, sideW - 28, 36 }; y += 40; } y = H - 20 - 3 * 40; for (int i = 4; i < 7; ++i) { navR[i] = { 14, y, sideW - 28, 36 }; y += 40; } }
        const int cw = 196, ch = 152, gap = 16, cols = jmax(1, (mw + gap) / (cw + gap)), top = 92;
        for (int i = 0; i < kTemplates; ++i) cardR[i] = { mx + (i % cols) * (cw + gap), top + (i / cols) * (ch + gap), cw, ch };
        const int rows = (kTemplates + cols - 1) / cols; formY = top + rows * (ch + gap) + 6;
        nameEd.setBounds(mx + 118, formY, jmin(340, mw - 118), 30);
        pathR = { mx + 118, formY + 44, jmax(60, jmin(420, mw - 118 - 100)), 30 }; folderB.setBounds(pathR.getRight() + 8, formY + 44, 92, 30);
        const int tw = jmax(80, jmin(260, mw - 118)); tempoSl.setBounds(mx + 118, formY + 88, tw, 30); sigLblX = mx + 118 + tw + 24; sigBox.setBounds(sigLblX + 112, formY + 88, 96, 30);
        createB.setBounds(W - 36 - 160, H - 36 - 42, 160, 42);
        search.setBounds(W - 36 - 260, 30, 260, 30); list.setBounds(mx, 84, mw, jmax(60, H - 84 - 90)); openB.setBounds(mx, H - 36 - 38, 120, 38); clearB.setBounds(mx + 132, H - 36 - 38, 120, 38); }
    static void drawGlyph(Graphics& g, int i, Rectangle<float> a) {
        const auto c = a.getCentre(); g.setColour(Colours::white.withAlpha(0.92f));
        switch (i) {
            case 0: { Path o, d; o.addRoundedRectangle(c.x - 24.f, c.y - 18.f, 48.f, 36.f, 7.f); const float dash[] = { 4.f, 3.f }; PathStrokeType(1.6f).createDashedStroke(d, o, dash, 2); g.fillPath(d); g.fillRect(c.x - 8.f, c.y - 1.f, 16.f, 2.f); g.fillRect(c.x - 1.f, c.y - 8.f, 2.f, 16.f); break; }
            case 1: { for (int k = 0; k < 7; ++k) g.fillRoundedRectangle(c.x - 28.f + (float) k * 8.2f, c.y - 16.f, 7.2f, 32.f, 1.5f);
                g.setColour(Colour(0xff1d1e22)); for (int k : { 0, 1, 3, 4, 5 }) g.fillRoundedRectangle(c.x - 28.f + (float) (k + 1) * 8.2f - 3.4f, c.y - 16.f, 5.6f, 19.f, 1.2f); break; }
            case 2: { static const float hs[9] = { 8.f, 16.f, 26.f, 34.f, 22.f, 30.f, 14.f, 24.f, 10.f }; for (int k = 0; k < 9; ++k) g.fillRoundedRectangle(c.x - 27.f + (float) k * 6.2f, c.y - hs[k] * 0.5f, 3.6f, hs[k], 1.8f); break; }
            case 3: { for (int r = 0; r < 2; ++r) for (int k = 0; k < 4; ++k) { g.setColour(Colours::white.withAlpha(((r * 4 + k) % 3 == 0) ? 0.95f : 0.45f)); g.fillRoundedRectangle(c.x - 27.f + (float) k * 14.f, c.y - 16.f + (float) r * 18.f, 12.f, 14.f, 3.f); } break; }
            case 4: { static const float ky[4] = { 6.f, -8.f, 10.f, -2.f }; for (int k = 0; k < 4; ++k) { const float x = c.x - 21.f + (float) k * 14.f; g.fillRect(x - 1.f, c.y - 17.f, 2.f, 34.f); g.fillRoundedRectangle(x - 5.f, c.y + ky[k] - 3.f, 10.f, 6.f, 2.f); } break; }
            default: { Path s; s.startNewSubPath(c.x - 27.f, c.y); for (int k = 1; k <= 54; ++k) s.lineTo(c.x - 27.f + (float) k, c.y - 15.f * std::sin((float) k / 54.f * MathConstants<float>::twoPi * 1.5f)); g.strokePath(s, PathStrokeType(2.8f, PathStrokeType::curved, PathStrokeType::rounded)); break; } } }
    static void drawNavIcon(Graphics& g, int i, Rectangle<float> r, Colour col) {
        g.setColour(col); const auto c = r.getCentre(); Path p;
        switch (i) {
            case 0: g.fillRect(c.x - 6.5f, c.y - 1.f, 13.f, 2.f); g.fillRect(c.x - 1.f, c.y - 6.5f, 2.f, 13.f); break;
            case 1: g.drawEllipse(c.x - 7.f, c.y - 7.f, 14.f, 14.f, 1.6f); g.drawLine(c.x, c.y, c.x, c.y - 4.5f, 1.5f); g.drawLine(c.x, c.y, c.x + 3.5f, c.y + 1.5f, 1.5f); break;
            case 2: p.addRoundedRectangle(c.x - 7.f, c.y - 4.f, 14.f, 11.f, 2.f); g.strokePath(p, PathStrokeType(1.5f)); g.fillRect(c.x - 7.f, c.y - 7.f, 6.f, 3.f); break;
            case 3: p.addCentredArc(c.x, c.y, 6.5f, 6.5f, 0.f, 0.8f, MathConstants<float>::twoPi - 0.3f, true); g.strokePath(p, PathStrokeType(1.6f)); { Path h; h.addTriangle(c.x + 1.f, c.y - 10.f, c.x + 1.f, c.y - 3.f, c.x + 6.f, c.y - 6.5f); g.fillPath(h); } break;
            case 4: g.drawEllipse(c.x - 4.f, c.y - 4.f, 8.f, 8.f, 1.6f); for (int k = 0; k < 8; ++k) { const float a = (float) k * MathConstants<float>::pi / 4.f; g.drawLine(c.x + 5.5f * std::sin(a), c.y - 5.5f * std::cos(a), c.x + 8.f * std::sin(a), c.y - 8.f * std::cos(a), 2.f); } break;
            case 5: g.fillRect(c.x - 7.f, c.y - 2.5f, 4.f, 5.f); p.addTriangle(c.x - 3.f, c.y - 2.5f, c.x - 3.f, c.y + 2.5f, c.x + 2.f, c.y + 6.f); p.addTriangle(c.x - 3.f, c.y - 2.5f, c.x - 3.f, c.y + 2.5f, c.x + 2.f, c.y - 6.f); g.fillPath(p); p.clear(); p.addCentredArc(c.x + 2.f, c.y, 6.f, 6.f, 0.f, 0.9f, 2.2f, true); g.strokePath(p, PathStrokeType(1.4f)); break;
            default: g.fillRoundedRectangle(c.x - 5.f, c.y - 3.f, 10.f, 9.f, 2.f); g.fillRect(c.x - 3.5f, c.y - 7.f, 2.f, 4.f); g.fillRect(c.x + 1.5f, c.y - 7.f, 2.f, 4.f); break; } }
    void paint(Graphics& g) override {
        const int W = getWidth(), H = getHeight(), mx = sideW + 36, mw = jmax(200, W - mx - 36); g.fillAll(Colour(0xff1d1e22));
        g.setColour(Colour(0xff25262b)); g.fillRect(0, 0, sideW, H); g.setColour(Colour(0xff111214)); g.fillRect(sideW - 1, 0, 1, H);
        { const auto lg = Rectangle<float>(20.f, 24.f, 44.f, 44.f); g.setGradientFill(ColourGradient(Colour(0xfff4f4f6), lg.getX(), lg.getY(), Colour(0xffa6a7af), lg.getRight(), lg.getBottom(), false)); g.fillRoundedRectangle(lg, 11.f);
          g.setColour(Colour(0xff1d1e22)); static const float hs[5] = { 10.f, 22.f, 30.f, 16.f, 8.f }; for (int k = 0; k < 5; ++k) g.fillRoundedRectangle(lg.getX() + 8.f + (float) k * 6.f, lg.getCentreY() - hs[k] * 0.5f, 3.6f, hs[k], 1.8f);
          g.setColour(Colour(0xfff2f2f5)); g.setFont(Font(FontOptions(20.f, Font::bold))); g.drawText("MiniDAW", 74, 24, sideW - 84, 26, Justification::centredLeft, false);
          g.setColour(Colour(0xff8a8b93)); g.setFont(Font(FontOptions(12.f))); g.drawText("Version 1.2.9", 74, 48, sideW - 84, 18, Justification::centredLeft, false); }
        { static const char* labels[7] = { "New Project", "Recent Projects", "Open Project...", "Open Auto-save Backup", "Settings", "Audio / MIDI", "Plug-in Manager" };
          g.setColour(Colour(0xff6b6c73)); g.setFont(Font(FontOptions(10.5f, Font::bold))); g.drawText("PROJECTS", 22, 90, 120, 16, Justification::centredLeft, false); g.drawText("SETTINGS", 22, navR[4].getY() - 22, 120, 16, Justification::centredLeft, false);
          for (int i = 0; i < 7; ++i) { const auto r = navR[i]; const bool act = (i == 0 && tab == NewTab) || (i == 1 && tab == RecentTab);
              if (act) { g.setColour(Colour(0x26ffffff)); g.fillRoundedRectangle(r.toFloat(), 8.f); } else if (i == hoverNav) { g.setColour(Colour(0x14ffffff)); g.fillRoundedRectangle(r.toFloat(), 8.f); }
              drawNavIcon(g, i, Rectangle<float>((float) r.getX() + 8.f, (float) r.getY() + 8.f, 20.f, 20.f), act ? Colours::white : Colour(0xffb4b5bd));
              g.setColour(act ? Colours::white : Colour(0xffd0d1d8)); g.setFont(Font(FontOptions(14.f, act ? Font::bold : Font::plain))); g.drawText(labels[i], r.getX() + 38, r.getY(), r.getWidth() - 44, r.getHeight(), Justification::centredLeft, true); } }
        if (tab == NewTab) {
            g.setColour(Colour(0xfff2f2f5)); g.setFont(Font(FontOptions(26.f, Font::bold))); g.drawText("Choose a Template", mx, 28, mw, 34, Justification::centredLeft, false);
            g.setColour(Colour(0xff8a8b93)); g.setFont(Font(FontOptions(13.5f))); g.drawText("Pick a starting point, name your project and press Create.", mx, 60, mw, 20, Justification::centredLeft, false);
            for (int i = 0; i < kTemplates; ++i) { const auto r = cardR[i].toFloat(); const auto& t = tpl(i); const bool s = i == tplSel;
                g.setColour(i == hoverCard && ! s ? Colour(0xff2f3036) : Colour(0xff28292e)); g.fillRoundedRectangle(r, 12.f);
                const auto art = Rectangle<float>(r.getX() + 6.f, r.getY() + 6.f, r.getWidth() - 12.f, 88.f); g.setGradientFill(ColourGradient(Colour(t.c1), art.getX(), art.getY(), Colour(t.c2), art.getRight(), art.getBottom(), false)); g.fillRoundedRectangle(art, 8.f);
                drawGlyph(g, i, art);
                g.setColour(Colour(0xfff2f2f5)); g.setFont(Font(FontOptions(14.5f, Font::bold))); g.drawText(t.name, (int) r.getX() + 12, (int) r.getY() + 98, (int) r.getWidth() - 24, 20, Justification::centredLeft, true);
                g.setColour(Colour(0xff8f9098)); g.setFont(Font(FontOptions(11.5f))); g.drawFittedText(t.sub, (int) r.getX() + 12, (int) r.getY() + 118, (int) r.getWidth() - 24, 30, Justification::topLeft, 2);
                if (s) { g.setColour(Colour(0xfff2f2f5)); g.drawRoundedRectangle(r.reduced(1.f), 12.f, 2.5f); } }
            g.setColour(Colour(0xffc9cad2)); g.setFont(Font(FontOptions(14.f)));
            g.drawText("Name", mx, formY, 110, 30, Justification::centredLeft, false); g.drawText("Save in", mx, formY + 44, 110, 30, Justification::centredLeft, false);
            g.drawText("Tempo", mx, formY + 88, 110, 30, Justification::centredLeft, false); g.drawText("Time Signature", sigLblX, formY + 88, 112, 30, Justification::centredLeft, false);
            g.setColour(Colour(0xff15161a)); g.fillRoundedRectangle(pathR.toFloat(), 5.f); g.setColour(Colour(0xff3a3b40)); g.drawRoundedRectangle(pathR.toFloat().reduced(0.5f), 5.f, 1.f);
            g.setColour(Colour(0xffd0d1d8)); g.setFont(Font(FontOptions(13.f))); g.drawText(dir.getFullPathName(), pathR.reduced(10, 0), Justification::centredLeft, true);
        } else {
            g.setColour(Colour(0xfff2f2f5)); g.setFont(Font(FontOptions(26.f, Font::bold))); g.drawText("Recent Projects", mx, 28, mw - 280, 34, Justification::centredLeft, false);
            const bool none = shown.empty(); const bool filtered = search.getText().isNotEmpty();
            if (none) { const auto lr = Rectangle<int>(mx, 84, mw, jmax(60, H - 84 - 90));
                g.setColour(Colour(0xff8f9097)); g.setFont(Font(FontOptions(17.f, Font::bold))); g.drawText(filtered ? "No matching projects" : "No recent projects yet", lr.translated(0, -12), Justification::centred, false);
                g.setColour(Colour(0xff6b6c73)); g.setFont(Font(FontOptions(13.f))); g.drawText(filtered ? "Try a different search." : "Projects you create or open will show up here.", lr.translated(0, 14), Justification::centred, false); } }
    }
    void mouseMove(const MouseEvent& e) override {
        int hc = -1, hn = -1; if (tab == NewTab) for (int i = 0; i < kTemplates; ++i) if (cardR[i].contains(e.getPosition())) hc = i; for (int i = 0; i < 7; ++i) if (navR[i].contains(e.getPosition())) hn = i;
        if (hc != hoverCard || hn != hoverNav) { hoverCard = hc; hoverNav = hn; setMouseCursor(hc >= 0 || hn >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor); repaint(); } }
    void mouseExit(const MouseEvent&) override { hoverCard = hoverNav = -1; repaint(); }
    void mouseDown(const MouseEvent& e) override {
        const auto pt = e.getPosition();
        for (int i = 0; i < 7; ++i) if (navR[i].contains(pt)) {
            if (i == 0) setTab(NewTab); else if (i == 1) setTab(RecentTab); else if (i == 2) { if (onOpenDialog) onOpenDialog(); } else if (i == 3) { if (onOpenBackup) onOpenBackup(); }
            else if (i == 4) { if (onSettings) onSettings(); } else if (i == 5) { if (onAudio) onAudio(); } else if (onPlugins) onPlugins(); return; }
        if (tab == NewTab) for (int i = 0; i < kTemplates; ++i) if (cardR[i].contains(pt)) { tplSel = i; repaint(); return; } }
    void mouseDoubleClick(const MouseEvent& e) override { if (tab == NewTab) for (int i = 0; i < kTemplates; ++i) if (cardR[i].contains(e.getPosition())) { tplSel = i; create(); return; } }
    bool keyPressed(const KeyPress& k) override {
        if (k == KeyPress::returnKey) { if (tab == NewTab) create(); else openRow(list.getSelectedRow()); return true; }
        if (k == KeyPress::escapeKey && tab == RecentTab && search.getText().isNotEmpty()) { search.clear(); return true; } return false; }
};
struct MainComponent : AudioAppComponent, Timer, MidiInputCallback, ChangeListener, MenuBarModel, FileDragAndDropTarget {
    DawLnF lnf; Project p; Engine eng { p }; std::unique_ptr<PluginWindow> pwin;
    Arrangement arr { p }; PianoRoll roll { p }; MixerPanel mix { p }; Inspector insp { p }; MasterStrip masterStrip { p }; Viewport mixView; ArrViewport arrView;
    IconButton rew { IconButton::Rewind }, stop { IconButton::Stop }, play { IconButton::Play }, rec { IconButton::Rec }, metro { IconButton::Metro }, inspB { IconButton::Inspector }, mixB { IconButton::Mixer }, rollB { IconButton::Editor }, cycB { IconButton::Cycle };
    TextButton addInst { "+ Inst" }, addAud { "+ Audio" }, scan { "Plugins" }, dev { "Audio/MIDI" }, exportB { "Export" };      
    Lcd lcd { p }; TunerPanel tuner { p }; IconButton tunerB { IconButton::Tuner }; bool tunerOn = false; Rectangle<int> viewsR, transR, metroR, volR; static constexpr int barH = 56;
    Slider tempo { Slider::LinearHorizontal, Slider::NoTextBox }, master { Slider::LinearHorizontal, Slider::NoTextBox };
    std::unique_ptr<FileChooser> chooser; std::shared_ptr<std::vector<float>> lastTake; std::vector<float> growBuf; bool growing = false;
    std::unique_ptr<MenuBarComponent> menuBar; PopupMenu appMenu; double uiScale = 1.0;
    StartScreen start; bool startOn = true, dirty = false; StringArray recent; int startupMode = 0, sigTick = 0; uint64 savedSig = 0;      
    File projDir = File::getSpecialLocation(File::userDocumentsDirectory).getChildFile("MiniDAW");          
    const String projExt = "mdp"; File projFile; int projGen = 0; static constexpr const char* kMagic = "MINIDAW-PROJECT-2"; static constexpr const char* kMagic3 = "MDAWPRJ3";   // v3 = raw audio blobs
    AudioBuffer<float> inBuf { 8, 4096 }; std::atomic<int> nInActive { 0 }, cbCount { 0 }, lastN { 0 }; std::atomic<bool> testTone { false }; std::atomic<float> outPeak { 0 }; std::atomic<int64> sampCount { 0 }; double tonePh = 0; AudioBuffer<float> stereoTmp { 2, 4096 }; bool inputTried = false, outFixTried = false; int inTick = 0; double pend[128]; int pendV[128] {}; bool inspOn = true, mixOn = true, rollOn = false;
    std::vector<double> taps; double lastAutosave = Time::getMillisecondCounterHiRes() * 0.001;
    static constexpr int kSigs[8] = { 2, 3, 4, 5, 6, 7, 9, 12 }; static constexpr int kPal = 10;
    static Colour pal(int i) { static const uint32 c[kPal] = { 0xffff4500, 0xff00bfff, 0xff3cb371, 0xffda70d6, 0xffffc233, 0xffff6f91, 0xff8e7cf3, 0xff2dd4bf, 0xffa3e635, 0xffb0b4bd }; return Colour(c[jlimit(0, kPal - 1, i)]); }
    static const char* palName(int i) { static const char* n[kPal] = { "Red-Orange", "Sky Blue", "Green", "Orchid", "Amber", "Pink", "Violet", "Teal", "Lime", "Silver" }; return n[jlimit(0, kPal - 1, i)]; }
    static Colour accentColour(int i) { static const uint32 c[6] = { 0xff5b8def, 0xff35c759, 0xfff59e0b, 0xff8b5cf6, 0xffec4899, 0xff14b8a6 }; return Colour(c[jlimit(0, 5, i)]); }
    static int autoMinutes(int i) { static const int v[5] = { 0, 1, 2, 5, 10 }; return v[jlimit(0, 4, i)]; }
    static File autosaveFile() { auto d = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("MiniDAW"); d.createDirectory(); return d.getChildFile("autosave.mdp"); }
    static File listFile() { auto d = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("MiniDAW"); d.createDirectory(); return d.getChildFile("plugins.xml"); }
    static void noFocus(Component* c) { for (auto* ch : c->getChildren()) { if (dynamic_cast<StartScreen*>(ch) != nullptr) continue; if (dynamic_cast<TextEditor*>(ch) == nullptr) { ch->setMouseClickGrabsKeyboardFocus(false); ch->setWantsKeyboardFocus(false); } noFocus(ch); } }
    MainComponent() {
        LookAndFeel::setDefaultLookAndFeel(&lnf); auto& lf = lnf; lf.setColourScheme(LookAndFeel_V4::getMidnightColourScheme());
        lf.setColour(ResizableWindow::backgroundColourId, Colour(0xff2b2c30)); lf.setColour(TextButton::buttonColourId, Colour(0xff4a4b50)); lf.setColour(TextButton::textColourOffId, Colour(0xffe4e4e8)); lf.setColour(TextButton::textColourOnId, Colours::white); lf.setColour(TextButton::buttonOnColourId, Colour(0xff5b8def));
        lf.setColour(Slider::trackColourId, Colour(0xff5b8def)); lf.setColour(Slider::thumbColourId, Colours::white); lf.setColour(Slider::backgroundColourId, Colour(0xff1b1c1f)); lf.applyTheme(Colour(0xff5b8def));
        p.fm.addDefaultFormats(); loadSettings();
        // ── NEW: pick up any .mdpd pedals from ~/Documents/MiniDAW/Pedals/ ──
        {
            const auto dir = File::getSpecialLocation(File::userDocumentsDirectory)
                                 .getChildFile("MiniDAW/Pedals").getFullPathName().toStdString();
            ampsim::loadCustomPedalDir(dir);
        }
        if (auto xml = parseXML(listFile())) p.known.recreateFromXml(*xml);
        p.known.addChangeListener(this);
        for (Component* c : std::initializer_list<Component*> { &arrView, &roll, &mixView, &masterStrip, &insp, &rew, &stop, &play, &rec, &metro, &inspB, &mixB, &rollB, &addInst, &addAud, &cycB, &lcd, &master, &tuner, &tunerB, &start }) addAndMakeVisible(c);
        tuner.setVisible(false); tuner.onClose = [this] { tunerB.triggerClick(); }; roll.onChange = [this] { saveSettings(); lcd.repaint(); };
        mixView.setViewedComponent(&mix, false); mixView.setScrollBarsShown(false, true); arrView.setViewedComponent(&arr, false);
        arr.onSelect = [this] { insp.refresh(); roll.repaint(); }; mix.onSelect = arr.onSelect;
        arr.onRename = [this](int ti) { p.selTrack = ti; p.selClip = -1; renameTrack(); };
        mix.onTrackOp = insp.onTrackOp = [this](int op) { Component::SafePointer<MainComponent> sp(this);      
            MessageManager::callAsync([sp, op] { if (sp == nullptr) return; if (op == 1) sp->renameTrack(); else if (op == 2) sp->duplicateTrack(); else if (op == 3) sp->deleteTrack(sp->p.selTrack); }); };
        mix.onSlot = [this](Track& t, int s) { slotMenu(t, s); }; insp.onSlot = [this](Track& t, int s) { slotMenu(t, s); }; insp.onChange = [this] { refreshAll(); };
        insp.onDelete = [this] { deleteTrack(p.selTrack); };
        masterStrip.onSlot = [this](int s) { slotMenu(p.masterTrack, s); }; masterStrip.onOpen = [this](int s) { openEditor(p.masterTrack.fx[(size_t) s].get()); };
        metro.addMouseListener(this, false);        
        mix.onOpen = insp.onOpen = [this](Track& t, int s) { if (s >= 0 && t.hasAmp(s)) openAmpSim(t); else openEditor((s < 0 ? t.plugin : t.fx[s]).get()); };
        arr.onCmd = [this](int id) { doMenu(id); }; arr.onZoom = [this] { resized(); };
        arr.onMenu = [this](int ti) { PopupMenu m; m.addItem(1, "Record Enable"); m.addItem(3, "Color & Icon..."); m.addSeparator(); m.addItem(2, "Delete Track");
            m.showMenuAsync(PopupMenu::Options(), [this, ti](int r) { if (ti >= (int) p.tracks.size()) return;
                if (r == 1) { ScopedLock l(p.lock); p.tracks[(size_t) ti]->armed = ! p.tracks[(size_t) ti]->armed; } else if (r == 2) deleteTrack(ti); else if (r == 3) arr.openLook(ti); }); };
        rew.onClick = [this] { p.beat = 0; };
        play.onClick = [this] { setPlaying(! p.playing); }; stop.onClick = [this] { rec.setToggleState(false, dontSendNotification); p.rec = false; setPlaying(false); p.beat = 0; };
        for (auto* b : { &rec, &metro, &inspB, &mixB, &rollB, &tunerB }) b->setClickingTogglesState(true);
        inspB.setToggleState(true, dontSendNotification); mixB.setToggleState(true, dontSendNotification);
        rec.onClick = [this] { p.rec = rec.getToggleState(); if (! p.rec) setPlaying(false); else if (! p.playing) setPlaying(true); else { ScopedLock l(p.lock); beginRec(); } };   
        metro.onClick = [this] { p.metro = metro.getToggleState(); };      
        inspB.onClick = [this] { inspOn = inspB.getToggleState(); resized(); }; mixB.onClick = [this] { mixOn = mixB.getToggleState(); resized(); }; rollB.onClick = [this] { rollOn = rollB.getToggleState(); resized(); }; tunerB.onClick = [this] { tunerOn = tunerB.getToggleState(); resized(); };
        exportB.onClick = [this] { exportDialog(); };
        cycB.setClickingTogglesState(true);
        cycB.onClick = [this] { p.cycle = cycB.getToggleState();                       
            if (p.cycle) { if (auto* c = p.selected()) { p.loopStart = c->start; p.loopEnd = c->start + c->len; } else { p.loopStart = 0; p.loopEnd = 16; } } };
        addInst.onClick = [this] { instrumentChooser(); }; addAud.onClick = [this] { p.addTrack(true); refreshAll(); };
        scan.onClick = [this] { auto* c = new PluginManagerPanel(p.fm, p.known, listFile().getSiblingFile("dead.txt")); c->setSize(660, 540); launch(c, "Plugin Manager"); };
        dev.onClick = [this] { launch(new DiagPanel(*this), "Audio / MIDI Settings + Diagnostics"); };
        tempo.setRange(20, 300, 0.1); tempo.setDoubleClickReturnValue(true, 120.0); tempo.setValue(120); tempo.onValueChange = [this] { p.bpm = tempo.getValue(); };
        master.getProperties().set("logic", true); master.setRange(0, 1.2, 0.01); master.setDoubleClickReturnValue(true, 0.9); master.setValue(0.9); master.onValueChange = [this] { p.master = (float) master.getValue(); };
        lcd.cpu = [this] { return (float) deviceManager.getCpuUsage(); };
        lcd.onTempo = [this](double v) { setTempo(v); }; lcd.onTempoEdit = [this] { tempoDialog(); }; lcd.onTap = [this] { tapTempo(); };
        lcd.onGrid = [this] { p.noteSnap = (p.noteSnap + 1) % 5; saveSettings(); lcd.repaint(); }; lcd.onInOut = [this] { cycB.triggerClick(); };
        lcd.onSig = [this] { PopupMenu m; for (int i = 0; i < 8; ++i) m.addItem(100 + i, String(kSigs[i]) + "/4", true, kSigs[i] == p.beatsPerBar.load());
            m.showMenuAsync(PopupMenu::Options().withTargetComponent(&lcd), [this](int r) { if (r >= 100) { p.beatsPerBar = kSigs[r - 100]; arr.repaint(); roll.repaint(); } }); };
        lcd.onMenu = [this] { PopupMenu m; m.addItem(1, "Metronome Settings..."); m.addItem(2, "Count-in: " + (p.countBars == 0 ? String("Off") : String(p.countBars) + (p.countBars == 1 ? " bar" : " bars")) + " (click to cycle)");
            m.addSeparator(); m.addItem(3, "Plug-in Manager..."); m.addItem(4, "Audio / MIDI Settings..."); m.addItem(5, "Preferences..."); m.addSeparator(); m.addItem(6, "Export...");
            m.showMenuAsync(PopupMenu::Options().withTargetComponent(&lcd), [this](int r) {
                if (r == 1) metroDialog(); else if (r == 2) { p.countBars = (p.countBars + 1) % 5; saveSettings(); } else if (r == 3) scan.onClick(); else if (r == 4) dev.onClick(); else if (r == 5) prefsDialog(); else if (r == 6) exportB.onClick(); }); };
        std::fill(pend, pend + 128, -1.0); setWantsKeyboardFocus(true);
        start.recent = &recent; start.dir = projDir; start.setAccent(accentColour(p.accent));
        start.onOpenFile = [this](const File& f) { readProject(f); }; start.onOpenDialog = [this] { openProject(); }; start.onOpenBackup = [this] { openBackup(); };
        start.onSettings = [this] { prefsDialog(); }; start.onAudio = [this] { dev.onClick(); }; start.onPlugins = [this] { scan.onClick(); };
        start.onClearRecent = [this] { recent.clear(); saveSettings(); start.refresh(); };
        start.onRemoveRecent = [this](const File& f) { recent.removeString(f.getFullPathName()); saveSettings(); start.refresh(); };
        start.onDirChanged = [this](const File& d) { projDir = d; saveSettings(); };
        start.onCreate = [this](int t, const String& n, const File& d, double bpm, int sig) { createProject(t, n, d, bpm, sig); };
        setSize(1360, 780); setAudioChannels(0, 2);        
        if (deviceManager.getCurrentAudioDevice() == nullptr) AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "No audio output", "MiniDAW could not open an audio output device. Open Audio/MIDI settings and choose one.");
        refreshAll(); savedSig = signature();
        for (auto& d : MidiInput::getAvailableDevices()) deviceManager.setMidiInputDeviceEnabled(d.identifier, true);
        deviceManager.addMidiInputDeviceCallback({}, this); startTimerHz(30);
        showStart();
        if (startupMode == 1 && recent.size() > 0 && File(recent[0]).existsAsFile()) readProject(File(recent[0]));      
#if JUCE_MAC
        add(appMenu, 80, "About MiniDAW"); add(appMenu, 69, "Preferences...", "Cmd+,"); MenuBarModel::setMacMainMenu(this, &appMenu);   
#else
        menuBar = std::make_unique<MenuBarComponent>(this); addAndMakeVisible(*menuBar);
#endif
    }
    ~MainComponent() override { MenuBarModel::setMacMainMenu(nullptr); LookAndFeel::setDefaultLookAndFeel(nullptr); pwin.reset(); deviceManager.removeMidiInputDeviceCallback({}, this); shutdownAudio(); p.known.removeChangeListener(this); }
    void changeListenerCallback(ChangeBroadcaster*) override { if (auto xml = p.known.createXml()) xml->writeTo(listFile()); }
    void launch(Component* c, const String& title) {
        DialogWindow::LaunchOptions o; o.content.setOwned(c); o.dialogTitle = title; o.componentToCentreAround = this;
        o.dialogBackgroundColour = Colour(0xff25262b); o.useNativeTitleBar = false; o.resizable = true; if (auto* w = o.launchAsync()) w->setTitleBarHeight(36);
    }
    void refreshAll() { setTitle(); mix.rebuild(); insp.refresh(); resized(); arr.repaint(); noFocus(this); arr.setWantsKeyboardFocus(true); arr.setMouseClickGrabsKeyboardFocus(true); roll.setWantsKeyboardFocus(true); roll.setMouseClickGrabsKeyboardFocus(true); }
    struct DiagPanel : Component, Timer {
        MainComponent& m; AudioDeviceSelectorComponent sel; Label info; ToggleButton tone { "Engine test tone (440 Hz) - bypasses the project" }; TextButton copyB { "Copy report" }; int lastCb = 0, lastSkip = 0; int64 lastSamp = 0; double lastT = 0;
        explicit DiagPanel(MainComponent& mc) : m(mc), sel(mc.deviceManager, 0, 8, 0, 8, true, false, true, false) {
            info.setJustificationType(Justification::topLeft); info.setFont(Font(FontOptions(13.f))); info.setColour(Label::textColourId, Colours::white);
            tone.setColour(ToggleButton::textColourId, Colours::white); tone.onClick = [this] { m.testTone = tone.getToggleState(); };
            copyB.onClick = [this] { SystemClipboard::copyTextToClipboard(info.getText()); }; addAndMakeVisible(copyB); addAndMakeVisible(info); addAndMakeVisible(tone); addAndMakeVisible(sel); setSize(580, 840); startTimerHz(4); timerCallback(); }
        ~DiagPanel() override { m.testTone = false; }
        void resized() override { auto r = getLocalBounds().reduced(6); info.setBounds(r.removeFromTop(285)); { auto row = r.removeFromTop(26); copyB.setBounds(row.removeFromRight(110)); tone.setBounds(row); } sel.setBounds(r); }
        void timerCallback() override {
            auto* d = m.deviceManager.getCurrentAudioDevice(); String t; const int cb = m.cbCount.load(); const int perSec = (cb - lastCb) * 4; lastCb = cb;
            if (d == nullptr) t << "NO AUDIO DEVICE IS OPEN - choose an output below.\n";
            else { t << "Device: " << d->getName() << "  (" << d->getTypeName() << ")\nSample rate: " << String(d->getCurrentSampleRate(), 0) << " Hz   Buffer: " << d->getCurrentBufferSizeSamples() << " samples\n"
                     << "Output channels on: " << d->getActiveOutputChannels().countNumberOfSetBits() << "   Input channels on: " << d->getActiveInputChannels().countNumberOfSetBits() << "\n"; }
            t << "Audio callbacks per second: " << perSec << "   last block: " << m.lastN.load() << " samples\n"
              << "Playing: " << (m.p.playing ? "yes" : "no") << "   Metronome: " << (m.p.metro ? "on" : "off") << "   Master fader: " << String(Decibels::gainToDecibels(m.p.master.load()), 1) << " dB\n"
              << "Master peak L/R: " << String(Decibels::gainToDecibels(m.p.masterLevelL.load()), 1) << " / " << String(Decibels::gainToDecibels(m.p.masterLevelR.load()), 1) << " dB   (about -100 = silence)\n"
              << "Blocks skipped (UI held the lock): " << m.p.skipCount.load() << "\n"
              << "Level leaving the app: " << String(Decibels::gainToDecibels(m.outPeak.load()), 1) << " dB\n";
            const int sk = m.p.skipCount.load(); const bool skipping = sk - lastSkip > 5; lastSkip = sk;
            const float pk = jmax(m.p.masterLevelL.load(), m.p.masterLevelR.load()); const int nOut = d != nullptr ? d->getActiveOutputChannels().countNumberOfSetBits() : 0;
            const int64 sc = m.sampCount.load(); const double now = Time::getMillisecondCounterHiRes(), dt = (now - lastT) / 1000.0; double eff = 0;
            if (lastT > 0 && dt > 0.05) eff = (double) (sc - lastSamp) / dt; lastSamp = sc; lastT = now;
            const double devSr = d != nullptr ? d->getCurrentSampleRate() : 0.0;
            t << "Engine rate: " << String(m.p.sr, 0) << " Hz   Samples really processed per second: " << String(eff, 0) << "\n";
            t << "RESULT: ";
            if (d == nullptr) t << "no audio device is open - pick an output below.";
            else if (perSec < 5) t << "the device is not running (0 callbacks) - pick another output or sample rate below.";
            else if (devSr > 0 && std::abs(m.p.sr - devSr) > 1.0) t << "TIMING MISMATCH: the engine runs at " << String(m.p.sr, 0) << " Hz but the device at " << String(devSr, 0) << " Hz (it will resync itself).";
            else if (devSr > 0 && eff > 0 && (eff / devSr > 1.15 || eff / devSr < 0.85)) t << "TIMING MISMATCH: the device delivers " << String(eff, 0) << " samples/s but claims " << String(devSr, 0) << " Hz - the device is lying about its rate; pick another sample rate below.";
            else if (nOut < 2) t << "only " << nOut << " output channel is on - tick both outputs below.";
            else if (skipping) t << "the engine keeps skipping blocks because the UI holds the lock.";
            else if (! m.p.playing && ! m.testTone.load()) t << "press Play (space) with this window open to measure the engine.";
            else if (pk < 0.001f && ! m.testTone.load()) t << "the engine itself is silent: nothing is audible in the project (muted/empty track, master fader or master mute).";
            else if (m.outPeak.load() > 0.001f) t << "the app IS sending sound to '" << d->getName() << "' - the problem is the output device or macOS volume/output routing.";
            else t << "the engine makes signal but it is lost before the output.";
            info.setText(t, dontSendNotification); }
    };
    bool enableInputs() {
        auto* adev = deviceManager.getCurrentAudioDevice();
        if (adev != nullptr && adev->getActiveInputChannels().countNumberOfSetBits() > 0) return true;
        auto orig = deviceManager.getAudioDeviceSetup(), setup = orig;
        setup.useDefaultInputChannels = false; setup.inputChannels.clear(); setup.inputChannels.setRange(0, 2, true);
        if (auto* type = deviceManager.getCurrentDeviceTypeObject()) { auto names = type->getDeviceNames(true); const int di = type->getDefaultDeviceIndex(true); if (isPositiveAndBelow(di, names.size())) setup.inputDeviceName = names[di]; }
        const String err = deviceManager.setAudioDeviceSetup(setup, true); adev = deviceManager.getCurrentAudioDevice();
        if (err.isNotEmpty() || adev == nullptr || adev->getActiveInputChannels().countNumberOfSetBits() == 0) { deviceManager.setAudioDeviceSetup(orig, true); return false; }
        return true; }
    void fixOutputs() {                                              
        auto* d = deviceManager.getCurrentAudioDevice();
        if (d == nullptr || d->getActiveOutputChannels().countNumberOfSetBits() >= 2 || d->getOutputChannelNames().size() < 2) return;
        auto su = deviceManager.getAudioDeviceSetup(); su.useDefaultOutputChannels = false; su.outputChannels.clear(); su.outputChannels.setRange(0, 2, true);
        deviceManager.setAudioDeviceSetup(su, true); }
    void autoInputs() {                                              
        bool want = tuner.active.load(); for (auto& t : p.tracks) want |= (t->audio && t->armed);
        if (! want) { inputTried = false; return; }
        auto* adev = deviceManager.getCurrentAudioDevice();
        if (adev != nullptr && adev->getActiveInputChannels().countNumberOfSetBits() > 0) return;
        if (inputTried) return; inputTried = true; enableInputs(); }
    void beginRec() {                                                
        ScopedLock l(p.lock);
        if (p.recClip >= 0 || p.selTrack < 0 || p.selTrack >= (int) p.tracks.size()) return;
        auto& tr = *p.tracks[(size_t) p.selTrack]; Clip c { (double) p.beat, 0.25, {} }; c.rec = true;
        if (tr.audio) {                                              
            if (deviceManager.getCurrentAudioDevice() == nullptr || deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits() == 0) enableInputs();
            auto* adev = deviceManager.getCurrentAudioDevice(); const int nIn = adev ? adev->getActiveInputChannels().countNumberOfSetBits() : 0;
            if (nIn == 0 || tr.inCh >= nIn) { p.rec = false; rec.setToggleState(false, dontSendNotification);
                AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "No audio input",
                    "No input channel is available for this track. Open Audio/MIDI, choose your interface and enable an input channel. On macOS also allow microphone access in System Settings > Privacy & Security > Microphone."); return; }
            tr.armed = true; c.wavSr = p.sr; c.wav = std::make_shared<std::vector<float>>((size_t) (p.sr * 120.0));
            p.recNext = nullptr; growing = false; p.recFrom = c.start; p.recCap = (int) c.wav->size(); p.recPos = 0; p.recCh = tr.inCh; p.recPtr = c.wav->data(); }
        p.pushUndo(); tr.clips.push_back(c); p.recTrack = p.selTrack; p.recClip = (int) tr.clips.size() - 1; p.selClip = p.recClip;
    }
    void setPlaying(bool on) {
        ScopedLock l(p.lock);
        if (on && p.rec) { beginRec(); if (p.recClip >= 0 && p.countBars > 0 && ! p.playing) { p.recFrom = (double) p.beat; p.beat = p.beat - (double) p.beatsPerBar.load() * p.countBars; p.counting = true; } }
        if (! on && p.recClip >= 0) {
            auto& cl = p.tracks[(size_t) p.recTrack]->clips; auto& c = cl[(size_t) p.recClip]; c.rec = false; bool keep = true;
            if (c.wav) { p.recPtr = nullptr; while (p.recBusy) Thread::sleep(1); p.recNext = nullptr;
                if (growing) { if ((size_t) p.recCap == growBuf.size()) c.wav->swap(growBuf); growBuf = {}; growing = false; }       
                const int rp = p.recPos; keep = rp > (int) (c.wavSr * 0.2);
                if (keep) { c.wav->resize((size_t) rp); core::updatePeaks(c.wav->data(), rp, c.peaksDone, c.peaks); core::finishPeaks(c.wav->data(), rp, c.peaksDone, c.peaks);
                    c.len = (double) rp / c.wavSr * p.bpm / 60.0; lastTake = c.wav;
                    auto* ad = deviceManager.getCurrentAudioDevice();                           
                    const double latS = p.latencyAuto ? (ad ? (double) (ad->getInputLatencyInSamples() + ad->getOutputLatencyInSamples()) / c.wavSr : 0.0) : p.latencyMs / 1000.0;
                    c.start = jmax(0.0, c.start - latS * p.bpm / 60.0); } }
            else { lastTake.reset(); if (c.notes.empty()) keep = false; else c.len = jmax(1.0, std::ceil(jmax((double) p.beat, c.start + 1) - c.start)); }
            if (! keep) { cl.erase(cl.begin() + p.recClip); p.selClip = -1; }
            p.recClip = -1; }
        p.playing = on;
    }
    static File settingsFile() { auto d = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("MiniDAW"); d.createDirectory(); return d.getChildFile("settings.xml"); }
    void loadSettings() {
        auto x = parseXML(settingsFile()); if (! x) return;
        p.autoScroll = x->getIntAttribute("autoScroll", 1) != 0; p.countBars = x->getIntAttribute("countBars", 0); p.clickVol = (float) x->getDoubleAttribute("clickVol", 0.3);
        p.clickRecOnly = x->getIntAttribute("clickRecOnly", 0) != 0; p.latencyAuto = x->getIntAttribute("latencyAuto", 1) != 0; p.latencyMs = x->getIntAttribute("latencyMs", 0);
        p.showWave = x->getIntAttribute("showWave", 1) != 0; p.showNotes = x->getIntAttribute("showNotes", 1) != 0; uiScale = x->getDoubleAttribute("uiScale", 1.0);
        p.clickType = x->getIntAttribute("clickType", 0); p.clickSub = jlimit(1, 4, x->getIntAttribute("clickSub", 1)); p.clickAccent = x->getIntAttribute("clickAccent", 1) != 0;
        p.clickHi = (float) x->getDoubleAttribute("clickHi", 1600.0); p.clickLo = (float) x->getDoubleAttribute("clickLo", 1000.0);
        p.noteSnap = jlimit(0, 4, x->getIntAttribute("noteSnap", 3)); p.noteLenIdx = jlimit(0, 3, x->getIntAttribute("noteLenIdx", 1)); p.regionSnap = jlimit(0, 3, x->getIntAttribute("regionSnap", 2));
        p.defVel = jlimit(1, 127, x->getIntAttribute("defVel", 100)); p.accent = jlimit(0, 5, x->getIntAttribute("accent", 0)); p.autoIdx = jlimit(0, 4, x->getIntAttribute("autoIdx", 0));
        startupMode = jlimit(0, 1, x->getIntAttribute("startupMode", 0)); const String pd = x->getStringAttribute("projDir"); if (pd.isNotEmpty() && File::isAbsolutePath(pd) && File(pd) != File::getSpecialLocation(File::userDocumentsDirectory).getChildFile("MiniDAW Projects")) projDir = File(pd);
        recent.clear(); for (auto* e : x->getChildWithTagNameIterator("RECENT")) { const String rp = e->getStringAttribute("path"); if (rp.isNotEmpty()) recent.addIfNotAlreadyThere(rp); }
        if (auto* se = x->getChildByName("STEMS")) stemCfg.load(*se);
        applyAccent(); Desktop::getInstance().setGlobalScaleFactor((float) uiScale);
    }
    void applyAccent() { const auto c = accentColour(p.accent); lnf.applyTheme(c); sendLookAndFeelChange(); start.setAccent(c); }
    void saveSettings() {
        XmlElement x("SETTINGS"); x.setAttribute("autoScroll", p.autoScroll ? 1 : 0); x.setAttribute("countBars", p.countBars); x.setAttribute("clickVol", (double) p.clickVol.load());
        x.setAttribute("clickRecOnly", p.clickRecOnly ? 1 : 0); x.setAttribute("latencyAuto", p.latencyAuto ? 1 : 0); x.setAttribute("latencyMs", p.latencyMs);
        x.setAttribute("showWave", p.showWave ? 1 : 0); x.setAttribute("showNotes", p.showNotes ? 1 : 0); x.setAttribute("uiScale", uiScale);
        x.setAttribute("clickType", p.clickType.load()); x.setAttribute("clickSub", p.clickSub.load()); x.setAttribute("clickAccent", p.clickAccent.load() ? 1 : 0);
        x.setAttribute("clickHi", (double) p.clickHi.load()); x.setAttribute("clickLo", (double) p.clickLo.load());
        x.setAttribute("noteSnap", p.noteSnap); x.setAttribute("noteLenIdx", p.noteLenIdx); x.setAttribute("regionSnap", p.regionSnap);
        x.setAttribute("defVel", p.defVel); x.setAttribute("accent", p.accent); x.setAttribute("autoIdx", p.autoIdx); x.setAttribute("startupMode", startupMode); x.setAttribute("projDir", projDir.getFullPathName());
        for (auto& rp : recent) x.createNewChildElement("RECENT")->setAttribute("path", rp);
        stemCfg.save(*x.createNewChildElement("STEMS"));
        x.writeTo(settingsFile());
    }
    void prefsDialog() {
        auto* tabs = new TabbedComponent(TabbedButtonBar::TabsAtTop); tabs->setSize(720, 540); tabs->setTabBarDepth(40); const Colour bg(0xff25262b);
        auto* gen = new PrefPage; gen->addToggle("Auto-scroll with the playhead", p.autoScroll, [this](bool v) { p.autoScroll = v; saveSettings(); });
        gen->addChoice("When MiniDAW starts", { "Show the project chooser", "Reopen the last project" }, startupMode, [this](int i) { startupMode = i; saveSettings(); });
        gen->addText("Default folder for new projects", projDir.getFullPathName(), [this](String s) { if (File::isAbsolutePath(s)) { projDir = File(s); start.dir = projDir; saveSettings(); } });
        gen->addSlider("Tempo (BPM)", 20, 300, 0.1, (double) p.bpm, [this](double v) { p.bpm = v; tempo.setValue(v, dontSendNotification); });
        gen->addSlider("Interface size", 0.8, 1.6, 0.05, uiScale, [this](double v) { uiScale = v; Desktop::getInstance().setGlobalScaleFactor((float) v); saveSettings(); });
        gen->addChoice("Auto-save backup", { "Off", "Every minute", "Every 2 minutes", "Every 5 minutes", "Every 10 minutes" }, p.autoIdx, [this](int i) { p.autoIdx = i; saveSettings(); });
        auto* recP = new PrefPage; recP->addSlider("Count-in (bars)", 0, 4, 1, p.countBars, [this](double v) { p.countBars = (int) v; saveSettings(); });
        recP->addToggle("Automatic latency compensation", p.latencyAuto, [this](bool v) { p.latencyAuto = v; saveSettings(); });
        recP->addSlider("Manual latency (ms)", 0, 500, 1, p.latencyMs, [this](double v) { p.latencyMs = (int) v; saveSettings(); });
        auto* clickP = makeMetroPage();
        auto* disp = new PrefPage; disp->addToggle("Show waveforms in regions", p.showWave, [this](bool v) { p.showWave = v; saveSettings(); });
        disp->addToggle("Show note previews in regions", p.showNotes, [this](bool v) { p.showNotes = v; saveSettings(); });
        disp->addChoice("Accent colour", { "Blue", "Green", "Orange", "Purple", "Pink", "Teal" }, p.accent, [this](int i) { p.accent = i; applyAccent(); saveSettings(); });
        auto* edit = new PrefPage;
        edit->addChoice("Piano roll grid (note start)", { "Off", "1/4 note", "1/8 note", "1/16 note", "1/32 note" }, p.noteSnap, [this](int i) { p.noteSnap = i; saveSettings(); });
        edit->addChoice("Default new-note length", { "1/16 note", "1/8 note", "1/4 note", "1/2 note" }, p.noteLenIdx, [this](int i) { p.noteLenIdx = i; saveSettings(); });
        edit->addSlider("Default new-note velocity", 1, 127, 1, p.defVel, [this](double v) { p.defVel = (int) v; saveSettings(); });
        edit->addChoice("Region snap (arrangement)", { "Off", "1/2 beat", "1 beat", "1 bar" }, p.regionSnap, [this](int i) { p.regionSnap = i; saveSettings(); });
        tabs->addTab("General", bg, gen, true); tabs->addTab("Recording", bg, recP, true); tabs->addTab("Metronome", bg, clickP, true); tabs->addTab("Editing", bg, edit, true); tabs->addTab("Display", bg, disp, true);
        tabs->addTab("Audio / MIDI", bg, new AudioDeviceSelectorComponent(deviceManager, 0, 8, 0, 8, true, false, true, false), true);
        tabs->addTab("Plug-ins", bg, new PluginManagerPanel(p.fm, p.known, listFile().getSiblingFile("dead.txt")), true);
        launch(tabs, "Preferences");
    }
    PrefPage* makeMetroPage() {
        auto* pg = new PrefPage;
        pg->addToggle("Metronome on", p.metro, [this](bool v) { p.metro = v; metro.setToggleState(v, dontSendNotification); });
        pg->addChoice("Click sound", { "Beep", "Wood block", "Hi-hat tick", "Cowbell" }, p.clickType.load(), [this](int i) { p.clickType = i; saveSettings(); });
        pg->addSlider("Click volume", 0, 1, 0.01, p.clickVol, [this](double v) { p.clickVol = (float) v; saveSettings(); });
        pg->addSlider("Beats per bar (x/4)", 1, 12, 1, p.beatsPerBar.load(), [this](double v) { p.beatsPerBar = (int) v; arr.repaint(); roll.repaint(); });
        pg->addChoice("Subdivision", { "Quarter notes", "Eighth notes", "Eighth-note triplets", "Sixteenth notes" }, p.clickSub.load() - 1, [this](int i) { p.clickSub = i + 1; saveSettings(); });
        pg->addToggle("Accent the first beat of each bar", p.clickAccent.load(), [this](bool v) { p.clickAccent = v; saveSettings(); });
        pg->addSlider("Accent pitch (Hz)", 200, 4000, 10, p.clickHi.load(), [this](double v) { p.clickHi = (float) v; saveSettings(); });
        pg->addSlider("Beat pitch (Hz)", 200, 4000, 10, p.clickLo.load(), [this](double v) { p.clickLo = (float) v; saveSettings(); });
        pg->addToggle("Click only while recording", p.clickRecOnly, [this](bool v) { p.clickRecOnly = v; saveSettings(); });
        pg->addSlider("Count-in (bars)", 0, 4, 1, p.countBars, [this](double v) { p.countBars = (int) v; saveSettings(); });
        return pg; }
    void metroDialog() { auto* pg = makeMetroPage(); pg->setSize(620, 32 + 46 * pg->rowCount()); launch(pg, "Metronome Settings"); }
    void mouseDown(const MouseEvent& e) override { if (e.eventComponent == &metro && e.mods.isPopupMenu()) metroDialog(); }
    void setTempo(double bpm) { bpm = jlimit(20.0, 300.0, bpm); p.bpm = bpm; tempo.setValue(bpm, dontSendNotification); }
    void tapTempo() { taps.push_back(Time::getMillisecondCounterHiRes() * 0.001); if (taps.size() > 16) taps.erase(taps.begin()); const double bpm = core::tapTempo(taps); if (bpm > 0.0) setTempo(bpm); }
    void tempoDialog() {
        auto* aw = new AlertWindow("Set Tempo", "Beats per minute (20 - 300):", MessageBoxIconType::NoIcon); aw->addTextEditor("t", String(p.bpm.load(), 2)); aw->addButton("OK", 1); aw->addButton("Cancel", 0);
        aw->enterModalState(true, ModalCallbackFunction::create([this, aw](int r) { if (r == 1) { const double v = aw->getTextEditorContents("t").getDoubleValue(); if (v > 0.0) setTempo(v); } }), true); }
    void autosaveTick() {
        if (startOn) return;                                                                          
        const int mins = autoMinutes(p.autoIdx); if (mins <= 0 || p.playing || p.recClip >= 0) return;
        const double now = Time::getMillisecondCounterHiRes() * 0.001; if (now - lastAutosave < mins * 60.0) return; lastAutosave = now; writeProject(autosaveFile(), false, true); }
    void openBackup() { auto f = autosaveFile(); if (! f.existsAsFile()) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Auto-save", "There is no auto-save backup yet. Turn it on in Preferences > General."); return; }
        readProject(f); projFile = File(); savedSig ^= 1; dirty = true; setTitle(); }                                      
    void resetMaster() { ScopedLock l(p.lock); auto& m = p.masterTrack; for (auto& f : m.fx) f.reset(); m.pan = 0; m.mute = false; p.master = 0.9f; p.masterLimit = false; p.masterClip = false; }
    void saveMaster(XmlElement& root) {                                                       
        auto& m = p.masterTrack; auto* e = root.createNewChildElement("MASTER");
        e->setAttribute("vol", (double) p.master.load()); e->setAttribute("pan", (double) m.pan); e->setAttribute("mute", m.mute ? 1 : 0); e->setAttribute("limit", p.masterLimit.load() ? 1 : 0); e->setAttribute("eqOn", p.masterTrack.eqOn ? 1 : 0);
            p.masterTrack.eqSave(*e);
        for (int s = 0; s < 4; ++s) { auto* pl = m.fx[s].get(); if (! pl) continue; auto* pe = e->createNewChildElement("SLOT"); pe->setAttribute("slot", s);
            if (auto d = pl->getPluginDescription().createXml()) pe->addChildElement(d.release()); MemoryBlock mb; pl->getStateInformation(mb); pe->setAttribute("state", mb.toBase64Encoding()); } }
    void setVel(int v) { editNotes([v](Note& n) { n.vel = v; }); }
    void humanizeVel() { Random r; editNotes([&r](Note& n) { n.vel = jlimit(1, 127, n.vel + r.nextInt(25) - 12); }); }
    void panic() { ScopedLock l(p.lock); for (auto& tp : p.tracks) { for (auto& lv : tp->live) lv = false; if (tp->plugin) tp->coll.addMessageToQueue(MidiMessage::allNotesOff(1)); } std::fill(pend, pend + 128, -1.0); }
    void moveTrack(int dir) {
        const int a = p.selTrack, b = a + dir; if (a < 0 || b < 0 || a >= (int) p.tracks.size() || b >= (int) p.tracks.size() || p.recClip >= 0) return;
        pwin.reset(); arr.dragT = -1;
        { ScopedLock l(p.lock); std::swap(p.tracks[(size_t) a], p.tracks[(size_t) b]); p.undoS.clear(); p.redoS.clear(); p.selTrack = b; p.selClip = -1; p.recTrack = p.recClip = -1; }
        refreshAll(); }
    Track* selT() { return (p.selTrack >= 0 && p.selTrack < (int) p.tracks.size()) ? p.tracks[(size_t) p.selTrack].get() : nullptr; }
    void editNotes(const std::function<void(Note&)>& fn) { ScopedLock l(p.lock); auto* c = p.selected(); if (! c) return; p.pushUndo(); for (auto& n : c->notes) fn(n); }
    void splitSelected() {
        ScopedLock l(p.lock); auto* c = p.selected(); const double at = p.beat; if (! c || at <= c->start + 0.01 || at >= c->start + c->len - 0.01) return;
        if (c->wav) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Split", "Splitting audio regions is not supported yet. MIDI regions can be split."); return; }
        p.pushUndo(); Clip right(at, c->start + c->len - at); std::vector<Note> left;
        for (auto& n : c->notes) { const double s = c->start + n.start; if (s >= at) right.notes.push_back({ s - at, n.len, n.pitch, n.vel }); else { Note k = n; k.len = jmin(k.len, at - s); left.push_back(k); } }
        c->notes = left; c->len = at - c->start; p.tracks[(size_t) p.selTrack]->clips.push_back(right);
    }
    // ---- Logic-style editing / navigation helpers ----
    std::unique_ptr<Clip> clipboard;
    static constexpr double kFades[5] = { 0.0, 0.25, 0.5, 1.0, 2.0 };
    double regionStep() const { const double g = p.regionGrid(); return g > 0.0 ? g : 1.0; }
    Rectangle<int> centreArea() { return getScreenBounds().withSizeKeepingCentre(10, 10); }
    static void rebuildPeaks(Clip& c) {
        c.peaks.clear(); c.peaksDone = 0; if (! c.wav) return; const int n = (int) c.wav->size(); const float* d = c.wav->data(); std::vector<float> mix;
        if (c.wavR && (int) c.wavR->size() == n) { mix.resize((size_t) n); const float* r = c.wavR->data(); for (int i = 0; i < n; ++i) mix[(size_t) i] = std::abs(d[i]) >= std::abs(r[i]) ? d[i] : r[i]; d = mix.data(); }
        core::updatePeaks(d, n, c.peaksDone, c.peaks); core::finishPeaks(d, n, c.peaksDone, c.peaks); }
    void withRegion(const std::function<void(Clip&)>& fn) { ScopedLock l(p.lock); auto* c = p.selected(); if (c == nullptr || c->rec) return; p.pushUndo(); fn(*p.selected()); }
    void copyRegion(bool cut) { { ScopedLock l(p.lock); auto* c = p.selected(); if (c == nullptr || c->rec) return; clipboard = std::make_unique<Clip>(*c); } if (cut) deleteRegion(); }
    void pasteRegion() {
        ScopedLock l(p.lock); auto* tr = selT(); if (! clipboard || tr == nullptr || (clipboard->wav != nullptr) != tr->audio) return;
        p.pushUndo(); Clip c = *clipboard; c.start = jmax(0.0, core::floorBeat((double) p.beat.load(), p.regionGrid())); tr->clips.push_back(c); p.selClip = (int) tr->clips.size() - 1; }
    void nudgeRegion(int dir) { const double st = regionStep(); withRegion([dir, st](Clip& c) { c.start = jmax(0.0, c.start + (double) dir * st); }); }
    void muteRegion() { withRegion([](Clip& c) { c.mute = ! c.mute; }); }
    void setFade(bool in, double beats) { withRegion([in, beats](Clip& c) { if (in) c.fadeIn = jmin(beats, c.len); else c.fadeOut = jmin(beats, c.len); }); }
    void repeatRegion() {
        auto* aw = new AlertWindow("Repeat Region", "Number of copies:", MessageBoxIconType::NoIcon); aw->addTextEditor("n", "4"); aw->addButton("OK", 1); aw->addButton("Cancel", 0);
        aw->enterModalState(true, ModalCallbackFunction::create([this, aw](int r) { if (r != 1) return; const int n = jlimit(1, 64, aw->getTextEditorContents("n").getIntValue());
            withRegion([this, n](Clip& c) { auto& cl = p.tracks[(size_t) p.selTrack]->clips; const Clip src = c; for (int i = 1; i <= n; ++i) { Clip d = src; d.start = src.start + src.len * (double) i; cl.push_back(d); } }); }), true); }
    void normalizeRegion() { withRegion([](Clip& c) {
        if (! c.wav || c.wav->empty()) return; const bool st = c.wavR && c.wavR->size() == c.wav->size();
        const float pk = jmax(core::peakOf(c.wav->data(), (int) c.wav->size()), st ? core::peakOf(c.wavR->data(), (int) c.wavR->size()) : 0.f), g = core::normGain(pk, -1.f);
        auto nv = std::make_shared<std::vector<float>>(*c.wav); for (auto& x : *nv) x *= g; c.wav = nv;
        if (st) { auto nr = std::make_shared<std::vector<float>>(*c.wavR); for (auto& x : *nr) x *= g; c.wavR = nr; }
        rebuildPeaks(c); }); }
    void reverseRegion() { withRegion([this](Clip& c) {
        if (! c.wav || c.wav->empty()) return; const double tot = (double) c.wav->size() / (c.wavSr * 60.0 / p.bpm.load());
        auto nv = std::make_shared<std::vector<float>>(*c.wav); std::reverse(nv->begin(), nv->end()); c.wav = nv;
        if (c.wavR) { auto nr = std::make_shared<std::vector<float>>(*c.wavR); std::reverse(nr->begin(), nr->end()); c.wavR = nr; }
        c.offset = jmax(0.0, tot - c.offset - c.len); rebuildPeaks(c); }); }
    void selectRegion(int dir) {
        { ScopedLock l(p.lock); auto* tr = selT(); if (tr == nullptr || tr->clips.empty()) return;
          std::vector<int> idx; for (int i = 0; i < (int) tr->clips.size(); ++i) idx.push_back(i);
          std::sort(idx.begin(), idx.end(), [tr](int a, int b) { return tr->clips[(size_t) a].start < tr->clips[(size_t) b].start; });
          int pos = -1; for (int i = 0; i < (int) idx.size(); ++i) if (idx[(size_t) i] == p.selClip) pos = i;
          const int cnt = (int) idx.size(); pos = pos < 0 ? (dir > 0 ? 0 : cnt - 1) : (pos + dir + cnt) % cnt; p.selClip = idx[(size_t) pos]; }
        if (arr.onSelect) arr.onSelect(); }
    void selectTrackRel(int dir) { if (p.tracks.empty()) return; p.selTrack = jlimit(0, (int) p.tracks.size() - 1, p.selTrack + dir); p.selClip = -1; if (arr.onSelect) arr.onSelect(); arr.repaint(); }
    void gotoBar(int dir) { const int bpb = jmax(1, p.beatsPerBar.load()); const double b = p.beat, cur = std::floor(b / bpb);
        const double target = dir > 0 ? (cur + 1.0) * bpb : (b - cur * bpb > 0.01 ? cur * bpb : (cur - 1.0) * bpb); p.beat = jmax(0.0, target); }
    void addMarker() { const double b = p.beat; for (auto& m : p.markers) if (std::abs(m.first - b) < 0.01) return;
        p.markers.push_back({ b, "Marker " + String((int) p.markers.size() + 1) });
        std::sort(p.markers.begin(), p.markers.end(), [](const std::pair<double, String>& a, const std::pair<double, String>& c) { return a.first < c.first; }); arr.repaint(); }
    void gotoMarker(int dir) { const double b = p.beat;
        if (dir > 0) { for (auto& m : p.markers) if (m.first > b + 0.01) { p.beat = m.first; return; } }
        else for (size_t i = p.markers.size(); i-- > 0;) if (p.markers[i].first < b - 0.01) { p.beat = p.markers[i].first; return; } }
    int nearestMarker() const { int best = -1; double bd = 2.0; const double b = p.beat; for (size_t i = 0; i < p.markers.size(); ++i) { const double d = std::abs(p.markers[i].first - b); if (d <= bd) { bd = d; best = (int) i; } } return best; }
    void deleteMarker() { const int i = nearestMarker(); if (i >= 0) { p.markers.erase(p.markers.begin() + i); arr.repaint(); } }
    void renameMarker() { const int i = nearestMarker(); if (i < 0) return;
        auto* aw = new AlertWindow("Rename Marker", "", MessageBoxIconType::NoIcon); aw->addTextEditor("n", p.markers[(size_t) i].second); aw->addButton("OK", 1); aw->addButton("Cancel", 0);
        aw->enterModalState(true, ModalCallbackFunction::create([this, aw, i](int r) { if (r == 1 && i < (int) p.markers.size()) { p.markers[(size_t) i].second = aw->getTextEditorContents("n"); arr.repaint(); } }), true); }
    void loopOp(int op) { const double s = p.loopStart, e = p.loopEnd, len = jmax(0.25, e - s); const int bpb = jmax(1, p.beatsPerBar.load());
        if (op == 501) { if (auto* c = p.selected()) { p.loopStart = c->start; p.loopEnd = c->start + c->len; } }
        else if (op == 502) { const double b0 = std::floor((double) p.beat / bpb) * bpb; p.loopStart = b0; p.loopEnd = b0 + bpb; }
        else if (op == 503) p.loopEnd = s + len * 2.0; else if (op == 504) p.loopEnd = s + jmax(0.25, len * 0.5);
        else if (op == 505) { p.loopEnd = e + len; p.loopStart = s + len; } else if (op == 506 && s >= len) { p.loopStart = s - len; p.loopEnd = e - len; } }
    void resetMixer() { ScopedLock l(p.lock); for (auto& t2 : p.tracks) { t2->vol = 1.f; t2->pan = 0.f; } }
    void zoomFit() { double e = 8.0; { ScopedLock l(p.lock); for (auto& t2 : p.tracks) for (auto& c : t2->clips) e = jmax(e, c.start + c.len); }
        const double w = (double) arrView.getViewWidth() - (double) Arrangement::head - 30.0; if (w > 50.0) arr.ppb = jlimit(6.0, 96.0, w / e); resized(); }
    void duplicateRegion() { ScopedLock l(p.lock); if (auto* c = p.selected()) { p.pushUndo(); Clip d = *c; d.start += d.len; p.tracks[(size_t) p.selTrack]->clips.push_back(d); } }
    void deleteRegion() { ScopedLock l(p.lock); if (p.selected()) { p.pushUndo(); auto& cl = p.tracks[(size_t) p.selTrack]->clips; cl.erase(cl.begin() + p.selClip); p.selClip = -1; } }
    uint64 signature() {                                        
        ScopedLock l(p.lock); uint64 h = 1469598103934665603ULL; auto mixh = [&h](double v) { uint64 b = 0; std::memcpy(&b, &v, sizeof b); h = (h ^ b) * 1099511628211ULL; };
        mixh(p.bpm); mixh((double) p.beatsPerBar.load()); mixh((double) p.master.load()); mixh((double) p.tracks.size()); mixh((double) p.markers.size()); for (auto& mk : p.markers) { mixh(mk.first); mixh((double) mk.second.hashCode64()); } mixh((double) p.notes.hashCode64()); mixh((double) p.revRoom.load()); mixh((double) p.revDamp.load()); mixh((double) p.revReturn.load());
        for (auto& tp : p.tracks) { auto& t = *tp; mixh((double) t.name.hashCode64()); mixh((double) t.send); mixh((double) t.icon); mixh(t.eqOn ? 1.0 : 0.0); mixh((double) t.nEq); for (auto& bd : t.eq) { mixh((double) bd.freq); mixh((double) bd.gain); mixh((double) bd.q); mixh((double) bd.type); } mixh((double) t.vol); mixh((double) t.pan); mixh(t.mute ? 1.0 : 0.0); mixh(t.solo ? 1.0 : 0.0); mixh((double) t.preset);
            mixh((double) t.cutoff); mixh((double) t.atk); mixh((double) t.rel); mixh((double) t.colour.getARGB()); mixh((double) t.clips.size());
            if (t.ampSim) { auto& a = t.ampSim->params; mixh((double) t.ampSimSlot); mixh(t.ampSim->enabled ? 1.0 : 0.0);
                for (double v : { (double) a.input_gain, (double) a.drive, (double) a.asym, (double) a.bass_db, (double) a.mid_db, (double) a.treble_db, (double) a.cab_res_db, (double) a.cab_lp_hz, (double) a.presence_db, (double) a.master, a.noise_gate_enabled ? 1.0 : 0.0, (double) a.noise_gate_thresh_db, (double) a.voice, (double) a.tight, (double) a.sag, (double) a.mic, (double) a.reverb }) mixh(v);
                for (auto& pd : t.ampSim->pedalboard.pedals) { mixh((double) (int) pd->kind); mixh(pd->enabled ? 1.0 : 0.0); mixh(pd->post ? 1.0 : 0.0); std::vector<std::pair<std::string, double>> kv(pd->params.begin(), pd->params.end()); std::sort(kv.begin(), kv.end()); for (auto& q : kv) mixh(q.second); } }
            for (auto& c : t.clips) { mixh(c.start); mixh(c.len); mixh(c.offset); mixh(c.wav ? (double) c.wav->size() : -1.0); for (auto& n : c.notes) { mixh(n.start); mixh(n.len); mixh((double) n.pitch); mixh((double) n.vel); } } }
        return h; }
    void updateDirty() { const bool d = signature() != savedSig; if (d != dirty) { dirty = d; setTitle(); } }
    void markClean() { savedSig = signature(); dirty = false; setTitle(); }
    void addRecent(const File& f) {
        if (f == autosaveFile()) return; recent.removeString(f.getFullPathName()); recent.insert(0, f.getFullPathName()); while (recent.size() > 20) recent.remove(recent.size() - 1);
        saveSettings(); start.refresh(); }
    void showStart(StartScreen::Tab t = StartScreen::NewTab) { startOn = true; start.dir = projDir; start.resetForm(); start.setTab(t); resized(); start.toFront(true); setTitle(); }
    void hideStart() { startOn = false; resized(); setTitle(); grabKeyboardFocus(); }
    void confirmDiscard(std::function<void()> proceed) {         
        if (startOn) { proceed(); return; }
        dirty = signature() != savedSig; if (! dirty) { proceed(); return; }
        const String nm = projFile != File() ? projFile.getFileName() : String("Untitled");
        auto* aw = new AlertWindow("Save changes to \"" + nm + "\"?", "Your changes will be lost if you don't save them.", MessageBoxIconType::QuestionIcon, this);
        aw->addButton("Save", 1, KeyPress(KeyPress::returnKey)); aw->addButton("Don't Save", 2); aw->addButton("Cancel", 0, KeyPress(KeyPress::escapeKey));
        aw->enterModalState(true, ModalCallbackFunction::create([this, proceed](int r) {
            if (r == 2) proceed(); else if (r == 1) { if (projFile != File()) { if (writeProject(projFile)) proceed(); } else saveProject(true, proceed); } }), true); }
    void clearProject() {                                        
        setPlaying(false); pwin.reset(); rec.setToggleState(false, dontSendNotification); p.rec = false;
        { ScopedLock l(p.lock); p.tracks.clear(); p.markers.clear(); p.notes = String(); p.undoS.clear(); p.redoS.clear(); p.selTrack = -1; p.selClip = -1; p.beat = 0; p.length = 128; p.beatsPerBar = 4; p.bpm = 120; projFile = File(); ++projGen; }
        resetMaster(); tempo.setValue(120, dontSendNotification); refreshAll(); markClean(); }
    void newProject() { confirmDiscard([this] { clearProject(); showStart(StartScreen::NewTab); }); }      
    void closeProject() { confirmDiscard([this] { clearProject(); showStart(startOn ? start.tab : StartScreen::NewTab); }); }
    void addTpl(const char* name, bool audio, int preset) { p.addTrack(audio); ScopedLock l(p.lock); auto& t = *p.tracks.back(); t.name = name; if (! audio) Project::applyPreset(t, preset); }
    void createProject(int tpl, const String& name, const File& dir, double bpm, int sig) {
        if (! dir.createDirectory().wasOk()) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "New Project", "Could not create the folder " + dir.getFullPathName()); return; }
        clearProject(); { ScopedLock l(p.lock); p.bpm = bpm; p.beatsPerBar = jlimit(1, 16, sig); } tempo.setValue(bpm, dontSendNotification);
        switch (tpl) {
            case 1: addTpl("Synth", false, 0); break;
            case 2: addTpl("Audio 1", true, 0); break;
            case 3: addTpl("Drums", false, 9); addTpl("Sub Bass", false, 7); break;
            case 4: addTpl("Keys", false, 5); addTpl("Bass", false, 1); addTpl("Drums", false, 9); addTpl("Vocals", true, 0); break;
            case 5: addTpl("Lead", false, 0); addTpl("Pad", false, 2); addTpl("Bass", false, 7); addTpl("Drums", false, 9); break;
            default: break; }
        { ScopedLock l(p.lock); if (tpl == 2 && ! p.tracks.empty()) p.tracks[0]->armed = true; p.selTrack = p.tracks.empty() ? -1 : 0; p.selClip = -1; }
        writeProject(dir.getNonexistentChildFile(name, "." + projExt, false));                  
        hideStart(); refreshAll(); }
    void duplicateTrack() {
        auto* t = selT(); if (! t) return; ScopedLock l(p.lock); auto nt = std::make_unique<Track>(); nt->name = t->name + " copy"; nt->audio = t->audio; nt->colour = t->colour;
        nt->vol = t->vol; nt->pan = t->pan; Project::applyPreset(*nt, t->preset); nt->wave = t->wave; nt->cutoff = t->cutoff; nt->atk = t->atk; nt->rel = t->rel; nt->clips = t->clips; nt->eqOn = t->eqOn; nt->icon = t->icon; nt->send = t->send; for (int k = 0; k < 6; ++k) nt->eq[k] = t->eq[k]; nt->nEq = t->nEq; nt->coll.reset(p.sr);
        if (t->ampSim) { nt->ampSim = std::make_unique<ampsim::AmpSimProcessor>(*t->ampSim); nt->ampSimSlot = t->ampSimSlot; }
        p.tracks.push_back(std::move(nt));
        p.undoS.clear(); p.redoS.clear(); p.selTrack = (int) p.tracks.size() - 1; p.selClip = -1; refreshAll();
    }
    void renameTrack() {
        auto* t = selT(); if (! t) return; auto* aw = new AlertWindow("Rename Track", "", MessageBoxIconType::NoIcon); aw->addTextEditor("n", t->name); aw->addButton("OK", 1); aw->addButton("Cancel", 0);
        aw->enterModalState(true, ModalCallbackFunction::create([this, aw](int r) { if (r == 1) if (auto* tr = selT()) { tr->name = aw->getTextEditorContents("n"); refreshAll(); } }), true);
    }
    bool importFile(const File& f, double atBeat, int trackIdx) {
        AudioFormatManager afm; afm.registerBasicFormats(); std::unique_ptr<AudioFormatReader> rd(afm.createReaderFor(f));
        if (! rd || rd->lengthInSamples <= 0 || rd->sampleRate <= 0) return false;
        const int n = (int) jmin((int64) rd->lengthInSamples, (int64) (rd->sampleRate * 3600.0)); AudioBuffer<float> b(jmax(1, (int) rd->numChannels), n); rd->read(&b, 0, n, 0, true, true);
        auto v = std::make_shared<std::vector<float>>((size_t) n); std::shared_ptr<std::vector<float>> vR;
        if (b.getNumChannels() >= 2) vR = std::make_shared<std::vector<float>>((size_t) n);
        for (int i = 0; i < n; ++i) { (*v)[(size_t) i] = b.getSample(0, i); if (vR) (*vR)[(size_t) i] = b.getSample(1, i); }   
        if (trackIdx < 0 || trackIdx >= (int) p.tracks.size() || ! p.tracks[(size_t) trackIdx]->audio) { p.addTrack(true); trackIdx = (int) p.tracks.size() - 1; }
        { ScopedLock l(p.lock); auto& tr = *p.tracks[(size_t) trackIdx]; p.selTrack = trackIdx; if (tr.clips.empty()) tr.name = f.getFileNameWithoutExtension(); p.pushUndo();
            Clip c(jmax(0.0, atBeat), (double) n / rd->sampleRate * p.bpm / 60.0); c.wav = v; c.wavR = vR; c.wavSr = rd->sampleRate; rebuildPeaks(c);
            tr.clips.push_back(c); p.selClip = (int) tr.clips.size() - 1;
            if (c.start + c.len + 8 > p.length) p.length = c.start + c.len + 32; }      
        return true;
    }
    void importAudio() {
        chooser = std::make_unique<FileChooser>("Import audio", File::getSpecialLocation(File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg");
        chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this](const FileChooser& fc) {
            auto f = fc.getResult(); if (f == File()) return; auto* t = selT();
            if (! importFile(f, p.beat, t && t->audio ? p.selTrack : -1)) AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Import", "Could not read " + f.getFileName());
            refreshAll(); });
    }
    bool dragOver = false;
    static bool isAudioFile(const String& path) { const File f(path); AudioFormatManager afm; afm.registerBasicFormats(); return f.existsAsFile() && afm.findFormatForFileExtension(f.getFileExtension()) != nullptr; }
    bool isInterestedInFileDrag(const StringArray& files) override { for (auto& f : files) if (isProjectFile(File(f))) return true; if (startOn) return false; for (auto& f : files) if (isAudioFile(f)) return true; return false; }
    void fileDragEnter(const StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit(const StringArray&) override { dragOver = false; repaint(); }
    void filesDropped(const StringArray& files, int x, int y) override {
        dragOver = false; repaint(); for (auto& f : files) if (isProjectFile(File(f))) { openExternal(File(f)); return; }
        if (startOn) return;
        const Point<int> pt(x, y); const bool onArr = arrView.isVisible() && arrView.getBounds().contains(pt); const auto ap = arr.getLocalPoint(this, pt);
        const double beat = onArr ? core::floorBeat(jmax(0.0, (double) (ap.x - Arrangement::head) / arr.ppb), 0.25) : (double) p.beat;
        int lane = (onArr && ap.y >= Arrangement::ruler) ? (ap.y - Arrangement::ruler) / Arrangement::laneH : -1; StringArray failed; bool any = false;
        for (auto& path : files) { if (! isAudioFile(path)) continue; const File f(path);
            if (importFile(f, beat, lane)) any = true; else failed.add(f.getFileName()); lane = -1; }
        if (any) refreshAll();
        if (failed.size() > 0) AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Import", "Could not read:\n" + failed.joinIntoString("\n"));
    }
    void paintOverChildren(Graphics& g) override {
        if (! dragOver) return; auto r = getLocalBounds(); g.setColour(Colour(0x265b8def)); g.fillRect(r); g.setColour(Colour(0xff5b8def)); g.drawRect(r, 3);
        g.setColour(Colours::white); g.setFont(Font(FontOptions(22.f, Font::bold))); g.drawText("Drop audio files to import", r, Justification::centred);
    }
    StemSettings stemCfg;
    void stemDialog() {
        Clip* c = p.selected(); auto* t = selT();
        if (! c || ! c->wav || ! t || c->rec) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Split into Stems", "Click an audio region in the arrangement first, then choose Edit > Split into Stems..."); return; }
        Clip tc; { const double sps = c->wavSr * 60.0 / p.bpm.load(); const int tot = (int) c->wav->size(), i0 = jlimit(0, tot, (int) std::lround(c->offset * sps)), i1 = jlimit(i0, tot, (int) std::lround((c->offset + c->len) * sps));
          tc.wavSr = c->wavSr; tc.wav = std::make_shared<std::vector<float>>(c->wav->begin() + i0, c->wav->begin() + i1);
          if (c->wavR && (int) c->wavR->size() == tot) tc.wavR = std::make_shared<std::vector<float>>(c->wavR->begin() + i0, c->wavR->begin() + i1); rebuildPeaks(tc); }
        if (tc.wav->empty()) return;
        auto* panel = new StemSplitterPanel(t->name, tc.wav, tc.wavSr, tc.peaks, stemCfg, tc.wavR);
        const Track* srcTrack = t; const double startBeat = c->start; const String base = t->name; Component::SafePointer<MainComponent> self(this);
        panel->onChanged = [self](const StemSettings& s) { if (self != nullptr) { self->stemCfg = s; self->saveSettings(); } };
        panel->onDone = [self, srcTrack, startBeat, base](std::vector<StemOut>& stems) { if (self != nullptr) self->addStems(stems, srcTrack, startBeat, base); };
        panel->setSize(740, 730); launch(panel, "Split into Stems");
    }
    void addStems(const std::vector<StemOut>& stems, const Track* src, double startBeat, const String& base) {      
        if (stems.empty()) return; const int first = (int) p.tracks.size();
        for (auto& s : stems) { p.addTrack(true); ScopedLock l(p.lock); auto& t = *p.tracks.back(); t.name = base + " - " + s.name; t.colour = s.colour; }
        { ScopedLock l(p.lock); p.pushUndo();
          for (size_t i = 0; i < stems.size(); ++i) { auto& tr = *p.tracks[(size_t) first + i]; auto& s = stems[i]; const int n = (int) s.wav->size();
              Clip c(startBeat, (double) n / s.sr * p.bpm / 60.0); c.wav = s.wav; c.wavR = s.wavR; c.wavSr = s.sr; rebuildPeaks(c); tr.clips.push_back(std::move(c)); }
          if (stemCfg.muteOriginal) for (auto& tp : p.tracks) if (tp.get() == src) tp->mute = true;
          p.selTrack = first; p.selClip = -1; }
        refreshAll(); arr.repaint();
    }
    StringArray getMenuBarNames() override { return { "File", "Edit", "Track", "Navigate", "View", "Options", "Window", "Help" }; }
    static void add(PopupMenu& m, int id, const String& text, const String& sc = {}, bool on = true, bool tick = false) { PopupMenu::Item it(text); it.itemID = id; it.shortcutKeyDescription = sc; it.isEnabled = on; it.isTicked = tick; m.addItem(it); }
    PopupMenu getMenuForIndex(int idx, const String&) override {
        PopupMenu m; const bool sel = p.selected() != nullptr, hasT = selT() != nullptr;
        if (idx == 0) { add(m, 10, "New Project...", "Cmd+N"); add(m, 11, "Open...", "Cmd+O");
            { PopupMenu rc; for (int i = 0; i < jmin(recent.size(), 10); ++i) add(rc, 200 + i, File(recent[i]).getFileName()); if (recent.size() > 0) { rc.addSeparator(); add(rc, 220, "Clear Menu"); } m.addSubMenu("Open Recent", rc, recent.size() > 0); }
            m.addSeparator(); add(m, 17, "Close Project", "Cmd+W", ! startOn); add(m, 12, "Save", "Cmd+S", ! startOn); add(m, 15, "Save As...", "Shift+Cmd+S", ! startOn); m.addSeparator(); add(m, 13, "Import Audio File...", "Cmd+I", ! startOn); add(m, 14, "Export / Bounce...", "Cmd+E", ! startOn); add(m, 520, "Export Selected Track as Audio...", "", hasT); add(m, 525, "Export All Tracks as Separate Files...", "", ! startOn && ! p.tracks.empty()); add(m, 526, "Export Selected Region as Audio...", "", sel);
            m.addSeparator(); add(m, 523, "Import MIDI File...", "", ! startOn); add(m, 521, "Export Selected Track as MIDI File...", "", hasT && ! selT()->audio); add(m, 522, "Export All Tracks as MIDI File...", "", ! p.tracks.empty()); m.addSeparator(); add(m, 16, "Open Auto-save Backup"); m.addSeparator(); add(m, 18, "Project Notes...", "", ! startOn); }
        else if (idx == 1) { add(m, 20, "Undo", "Cmd+Z", ! p.undoS.empty()); add(m, 21, "Redo", "Shift+Cmd+Z", ! p.redoS.empty()); m.addSeparator();
            add(m, 22, "Split at Playhead", "Cmd+T", sel); add(m, 23, "Duplicate Region", "D", sel); add(m, 24, "Delete Region", "Delete", sel); m.addSeparator();
            PopupMenu q, tp; add(q, 25, "1/4 note"); add(q, 26, "1/8 note"); add(q, 27, "1/16 note", "Q"); m.addSubMenu("Quantize", q, sel);
            add(tp, 28, "Up 1 semitone"); add(tp, 29, "Down 1 semitone"); add(tp, 30, "Up 1 octave"); add(tp, 31, "Down 1 octave"); m.addSubMenu("Transpose", tp, sel);
            PopupMenu vl; add(vl, 120, "Soft (60)"); add(vl, 121, "Medium (85)"); add(vl, 122, "Loud (110)"); add(vl, 123, "Max (127)"); m.addSubMenu("Set Velocity", vl, sel); add(m, 124, "Humanize Velocity", "", sel);
            const bool aud = sel && p.selected()->wav != nullptr && ! p.selected()->rec;
            m.addSeparator(); add(m, 450, "Cut Region", "Cmd+X", sel); add(m, 451, "Copy Region", "Cmd+C", sel); add(m, 452, "Paste Region at Playhead", "Cmd+V", clipboard != nullptr && hasT);
            m.addSeparator(); add(m, 453, "Mute / Unmute Region", "", sel, sel && p.selected()->mute); add(m, 454, "Nudge Region Left", "Opt+Left", sel); add(m, 455, "Nudge Region Right", "Opt+Right", sel); add(m, 456, "Repeat Region...", "", sel);
            add(m, 457, "Select Next Region", "Tab", hasT); add(m, 458, "Select Previous Region", "Shift+Tab", hasT);
            { PopupMenu fi, fo; static const char* fn[5] = { "None", "1/4 beat", "1/2 beat", "1 beat", "2 beats" };
              for (int i = 0; i < 5; ++i) { add(fi, 470 + i, fn[i], "", aud, aud && std::abs(p.selected()->fadeIn - kFades[i]) < 1.0e-6); add(fo, 480 + i, fn[i], "", aud, aud && std::abs(p.selected()->fadeOut - kFades[i]) < 1.0e-6); }
              m.addSubMenu("Fade In", fi, aud); m.addSubMenu("Fade Out", fo, aud); }
            add(m, 459, "Normalize Audio Region", "", aud); add(m, 460, "Reverse Audio Region", "", aud); add(m, 470, "Split Region at Playhead", "Cmd+B", sel);
            m.addSeparator(); add(m, 125, "Split into Stems...", "", aud); }
        else if (idx == 2) { add(m, 40, "New Instrument Track..."); add(m, 41, "New Audio Track"); m.addSeparator(); add(m, 42, "Duplicate Track", "", hasT); add(m, 43, "Delete Track", "Cmd+Delete", hasT); add(m, 44, "Rename Track...", "", hasT); m.addSeparator();
            add(m, 45, "Mute", "", hasT, hasT && selT()->mute); add(m, 46, "Solo", "", hasT, hasT && selT()->solo); add(m, 47, "Record Enable", "", hasT, hasT && selT()->armed); m.addSeparator();
            add(m, 130, "Move Track Up", "", hasT && p.selTrack > 0); add(m, 131, "Move Track Down", "", hasT && p.selTrack < (int) p.tracks.size() - 1);
            m.addSeparator(); add(m, 510, "Color & Icon...", "", hasT); add(m, 511, "Reset All Faders and Pans"); add(m, 524, "Bounce Track in Place", "", hasT);
            PopupMenu col; for (int i = 0; i < kPal; ++i) { PopupMenu::Item it(palName(i)); it.itemID = 140 + i; it.colour = pal(i); it.isEnabled = hasT; col.addItem(it); } m.addSubMenu("Track Colour", col, hasT); }
        else if (idx == 3) { add(m, 55, "Go to Beginning", "Return"); add(m, 56, "Go to End"); add(m, 490, "Previous Bar", ","); add(m, 491, "Next Bar", "."); add(m, 492, "Go to Loop Start"); add(m, 495, "Go to Selected Region Start", "", sel); m.addSeparator();
            add(m, 493, "Previous Track", "Up", hasT && p.selTrack > 0); add(m, 494, "Next Track", "Down", hasT && p.selTrack < (int) p.tracks.size() - 1); m.addSeparator();
            add(m, 496, "Create Marker", "'"); add(m, 497, "Previous Marker", "["); add(m, 498, "Next Marker", "]"); add(m, 499, "Delete Nearest Marker", "", ! p.markers.empty()); add(m, 500, "Rename Nearest Marker...", "", ! p.markers.empty());
            { PopupMenu mk; for (size_t i = 0; i < jmin((size_t) 64, p.markers.size()); ++i) add(mk, 600 + (int) i, p.markers[i].second + "  (bar " + String((int) std::floor(p.markers[i].first / jmax(1, p.beatsPerBar.load())) + 1) + ")"); m.addSubMenu("Go to Marker", mk, ! p.markers.empty()); }
            m.addSeparator(); add(m, 501, "Set Loop to Region", "", sel); add(m, 502, "Loop 1 Bar at Playhead"); add(m, 503, "Double Loop Length"); add(m, 504, "Halve Loop Length"); add(m, 505, "Move Loop Forward"); add(m, 506, "Move Loop Back"); }
        else if (idx == 4) { add(m, 50, "Inspector", "I", true, inspOn); add(m, 51, "Mixer", "X", true, mixOn); add(m, 52, "Editor", "P", true, rollOn); add(m, 58, "Tuner", "U", true, tunerOn); m.addSeparator();
            add(m, 53, "Zoom In", "+"); add(m, 54, "Zoom Out", "-"); m.addSeparator(); add(m, 55, "Go to Beginning", "Return"); add(m, 56, "Go to End"); add(m, 57, "Follow Playhead", "", true, p.autoScroll);
            m.addSeparator(); add(m, 514, "Show Waveforms", "", true, p.showWave); add(m, 515, "Show Notes in Regions", "", true, p.showNotes); add(m, 516, "Zoom to Fit Project", "Z"); }
        else if (idx == 5) { add(m, 60, "Metronome", "K", true, p.metro); add(m, 61, "Cycle", "C", true, p.cycle); add(m, 67, "Click Only While Recording", "", true, p.clickRecOnly); add(m, 100, "Metronome Settings..."); m.addSeparator();
            add(m, 102, "Set Tempo..."); add(m, 101, "Tap Tempo", "T"); PopupMenu ts; for (int i = 0; i < 8; ++i) add(ts, 110 + i, String(kSigs[i]) + " / 4", "", true, p.beatsPerBar.load() == kSigs[i]); m.addSubMenu("Time Signature", ts); m.addSeparator();
            PopupMenu ci; add(ci, 63, "Off", "", true, p.countBars == 0); add(ci, 64, "1 bar", "", true, p.countBars == 1); add(ci, 65, "2 bars", "", true, p.countBars == 2); add(ci, 66, "4 bars", "", true, p.countBars == 4); m.addSubMenu("Count-in", ci);
            add(m, 68, "Automatic Latency Compensation", "", true, p.latencyAuto); add(m, 105, "Master Soft Limiter", "", true, p.masterLimit.load()); add(m, 512, "Master EQ..."); add(m, 513, "Reverb Bus Settings..."); m.addSeparator();
            add(m, 106, "MIDI Panic (All Notes Off)"); add(m, 107, "Clear All Mutes and Solos"); m.addSeparator(); add(m, 69, "Preferences...", "Cmd+,"); }
        else if (idx == 6) { add(m, 70, "Minimize"); add(m, 71, "Full Screen"); m.addSeparator(); add(m, 72, "Plug-in Manager..."); add(m, 73, "Audio / MIDI Settings..."); }
        else { add(m, 80, "About MiniDAW"); add(m, 81, "Keyboard Shortcuts"); }
        return m;
    }
    void menuItemSelected(int id, int) override { doMenu(id); }
    void doMenu(int id) {
        if (id >= 200 && id < 220) { const int i = id - 200; if (i < recent.size()) { const File f(recent[i]); confirmDiscard([this, f] { if (f.existsAsFile()) readProject(f);
                else { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Open", "Can't find \"" + f.getFileName() + "\". It was removed from the list."); recent.removeString(f.getFullPathName()); saveSettings(); start.refresh(); } }); } return; }
        if (id == 220) { recent.clear(); saveSettings(); start.refresh(); return; }
        if (startOn && id != 10 && id != 11 && id != 16 && id != 69 && id != 70 && id != 71 && id != 72 && id != 73 && id != 80 && id != 81) return;      
        auto* tr = selT();
        switch (id) {
            case 10: newProject(); break; case 11: confirmDiscard([this] { openProject(); }); break; case 17: closeProject(); break; case 12: saveProject(false); break; case 15: saveProject(true); break; case 13: importAudio(); break; case 14: exportDialog(); break;
            case 20: p.undo(); refreshAll(); break; case 21: p.redo(); refreshAll(); break; case 22: splitSelected(); break; case 23: duplicateRegion(); break; case 24: deleteRegion(); break;
            case 25: editNotes([](Note& n) { n.start = std::round(n.start * 1) / 1; }); break; case 26: editNotes([](Note& n) { n.start = std::round(n.start * 2) / 2; }); break;
            case 27: editNotes([](Note& n) { n.start = std::round(n.start * 4) / 4; }); break;
            case 28: editNotes([](Note& n) { n.pitch = jlimit(0, 127, n.pitch + 1); }); break; case 29: editNotes([](Note& n) { n.pitch = jlimit(0, 127, n.pitch - 1); }); break;
            case 30: editNotes([](Note& n) { n.pitch = jlimit(0, 127, n.pitch + 12); }); break; case 31: editNotes([](Note& n) { n.pitch = jlimit(0, 127, n.pitch - 12); }); break;
            case 40: instrumentChooser(); break; case 41: addAud.triggerClick(); break; case 42: duplicateTrack(); break; case 43: deleteTrack(p.selTrack); break; case 44: renameTrack(); break;
            case 45: if (tr) { ScopedLock l(p.lock); tr->mute = ! tr->mute; } break; case 46: if (tr) { ScopedLock l(p.lock); tr->solo = ! tr->solo; } break; case 47: if (tr) { ScopedLock l(p.lock); tr->armed = ! tr->armed; } break;
            case 50: inspB.triggerClick(); break; case 51: mixB.triggerClick(); break; case 52: rollB.triggerClick(); break; case 58: tunerB.triggerClick(); break;
            case 53: arr.ppb = jmin(400.0, arr.ppb * 1.25); resized(); break; case 54: arr.ppb = jmax(2.0, arr.ppb / 1.25); resized(); break;
            case 55: p.beat = 0; break; case 56: { double e = 0; for (auto& t2 : p.tracks) for (auto& c : t2->clips) e = jmax(e, c.start + c.len); p.beat = e; break; }
            case 57: p.autoScroll = ! p.autoScroll; saveSettings(); break;
            case 60: metro.triggerClick(); break; case 61: cycB.triggerClick(); break; case 67: p.clickRecOnly = ! p.clickRecOnly; saveSettings(); break;
            case 63: p.countBars = 0; saveSettings(); break; case 64: p.countBars = 1; saveSettings(); break; case 65: p.countBars = 2; saveSettings(); break; case 66: p.countBars = 4; saveSettings(); break;
            case 68: p.latencyAuto = ! p.latencyAuto; saveSettings(); break; case 69: prefsDialog(); break;
            case 70: if (auto* w = dynamic_cast<DocumentWindow*>(getTopLevelComponent())) w->setMinimised(true); break;
            case 71: if (auto* w = dynamic_cast<DocumentWindow*>(getTopLevelComponent())) w->setFullScreen(! w->isFullScreen()); break;
            case 72: scan.triggerClick(); break; case 73: dev.triggerClick(); break;
            case 80: AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "About MiniDAW", "MiniDAW 1.2.2\nA digital audio workstation with MIDI + audio recording, VST3/AU hosting, a mixer, drum kit and synth instruments, undo/redo, bounce/export and project files.\n\nBuilt with " + SystemStats::getJUCEVersion() + "."); break;
            case 81: AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Keyboard Shortcuts", "Space  Play / stop\nR  Record      K  Metronome      C  Cycle      T  Tap tempo\nX  Mixer      I  Inspector      P  Piano roll      U  Tuner\n+ / -  Zoom      Return  Go to start\nPiano roll: click = add note, drag = move, drag edge = resize, right-click = delete,\n    Delete / arrows = remove / nudge the selected note, Cmd+wheel = zoom\nD  Duplicate region      Q  Quantize 1/16      Delete  Delete region\nCmd+Z / Shift+Cmd+Z  Undo / redo      Cmd+T  Split at playhead\nCmd+N / O / S / W  New / open / save / close project      Cmd+I  Import audio\nCmd+E  Export      Cmd+Delete  Delete track      Cmd+,  Preferences\n\nCmd+X / C / V  Cut / copy / paste region      Tab  Next region\n, / .  Previous / next bar      \'  Create marker      [ / ]  Previous / next marker\nM / S  Mute / solo track      Z  Zoom to fit\nOpt+Left / Right  Nudge region      Up / Down  Select track (arrangement)"); break;
            case 16: confirmDiscard([this] { openBackup(); }); break;
            case 100: metroDialog(); break; case 101: tapTempo(); break; case 102: tempoDialog(); break;
            case 105: p.masterLimit = ! p.masterLimit.load(); break; case 106: panic(); break;
            case 107: { ScopedLock l(p.lock); for (auto& t2 : p.tracks) { t2->mute = false; t2->solo = false; } p.masterTrack.mute = false; } break;
            case 120: setVel(60); break; case 121: setVel(85); break; case 122: setVel(110); break; case 123: setVel(127); break; case 124: humanizeVel(); break; case 125: stemDialog(); break;
            case 130: moveTrack(-1); break; case 131: moveTrack(1); break;
            case 520: exportDialog(6); break; case 525: exportDialog(3); break; case 526: exportDialog(5); break; case 521: exportMidi(false); break; case 522: exportMidi(true); break; case 523: importMidi(); break; case 524: bounceInPlace(); break;
            case 18: CallOutBox::launchAsynchronously(std::make_unique<NotesBox>(p), centreArea(), nullptr); break;
            case 450: copyRegion(true); refreshAll(); break; case 451: copyRegion(false); break; case 452: pasteRegion(); refreshAll(); break; case 453: muteRegion(); break;
            case 454: nudgeRegion(-1); break; case 455: nudgeRegion(1); break; case 456: repeatRegion(); break; case 457: selectRegion(1); break; case 458: selectRegion(-1); break;
            case 459: normalizeRegion(); break; case 460: reverseRegion(); break; case 470: arr.splitAtPlayhead(); refreshAll(); break;
            case 490: gotoBar(-1); break; case 491: gotoBar(1); break; case 492: p.beat = p.loopStart.load(); break; case 493: selectTrackRel(-1); break; case 494: selectTrackRel(1); break;
            case 495: if (auto* c = p.selected()) p.beat = c->start; break;
            case 496: addMarker(); break; case 497: gotoMarker(-1); break; case 498: gotoMarker(1); break; case 499: deleteMarker(); break; case 500: renameMarker(); break;
            case 501: case 502: case 503: case 504: case 505: case 506: loopOp(id); break;
            case 510: arr.openLook(p.selTrack); break; case 511: resetMixer(); break;
            case 512: CallOutBox::launchAsynchronously(std::make_unique<EqEditor>(p, p.masterTrack), centreArea(), nullptr); break;
            case 513: CallOutBox::launchAsynchronously(std::make_unique<RevSettings>(p), centreArea(), nullptr); break;
            case 514: p.showWave = ! p.showWave; saveSettings(); break; case 515: p.showNotes = ! p.showNotes; saveSettings(); break; case 516: zoomFit(); break;
            default:
                if (id >= 600 && id < 664) { const size_t mi = (size_t) (id - 600); if (mi < p.markers.size()) p.beat = p.markers[mi].first; }
                else if (id >= 470 && id < 475) setFade(true, kFades[id - 470]); else if (id >= 480 && id < 485) setFade(false, kFades[id - 480]);
                else if (id >= 110 && id <= 117) { p.beatsPerBar = kSigs[id - 110]; }
                else if (id >= 140 && id < 140 + kPal) { if (tr) { ScopedLock l(p.lock); tr->colour = pal(id - 140); } refreshAll(); }
                break; }
        if ((id >= 20 && id <= 47) || (id >= 110 && id <= 131)) arr.repaint();
    }
    void showPicker(const String& title, std::vector<PickItem> items, const StringArray& chips, std::vector<std::pair<String, int>> actions, std::function<void(int)> cb) {      
        auto c = std::make_unique<ChooserList>(); c->title = title; c->items = std::move(items); c->chips = chips; c->singleClick = true; c->emptyText = "Nothing to choose from"; c->setActions(std::move(actions));
        c->onPick = cb; c->onAction = cb; c->setSize(400, 540); c->rebuild(); const auto mp = Desktop::getMousePosition();
        CallOutBox::launchAsynchronously(std::move(c), Rectangle<int>(mp.x - 2, mp.y - 2, 4, 4), nullptr); }
    void instrumentChooser() {                                        
        auto types = p.known.getTypes(); std::vector<PickItem> items; Array<int> idx;
        struct B { int id; const char* name; const char* grp; };
        static const B bi[] = { { 5003, "Triangle Keys", "Keys" }, { 5005, "Electric Piano", "Keys" }, { 5006, "Organ", "Keys" }, { 5000, "Saw Lead", "Synth" }, { 5004, "Warm Saw", "Synth" }, { 5001, "Square Bass", "Bass" },
                                { 5007, "Sub Bass", "Bass" }, { 5002, "Sine Pad", "Pads" }, { 5008, "Strings", "Pads" }, { 5009, "Drum Kit", "Drums - kick 36, snare 38, hats 42/46" } };
        for (auto& b : bi) { PickItem it; it.id = b.id; it.name = b.name; it.sub = b.grp; it.badge = "Built-in"; it.chip = 1; items.push_back(it); }
        std::vector<int> order; for (int i = 0; i < types.size(); ++i) if (types[i].isInstrument) order.push_back(i);
        std::sort(order.begin(), order.end(), [&types](int a, int b) { return types[a].name.compareIgnoreCase(types[b].name) < 0; });
        for (int i : order) { PickItem it; it.id = idx.size() + 1; it.name = types[i].name; it.sub = types[i].manufacturerName; it.badge = types[i].pluginFormatName; it.chip = 2; items.push_back(it); idx.add(i); }
        showPicker("New Instrument Track", std::move(items), { "All", "Built-in", "Plug-ins" }, { { "Scan for Plug-ins...", 9002 } }, [this, types, idx](int r) {
            if (r == 9002) { scan.triggerClick(); return; } if (r == 0) return; p.addTrack(false); auto& t = *p.tracks.back();
            if (r >= 5000) { Project::applyPreset(t, r - 5000); t.name = Project::presets[r - 5000].name; refreshAll(); return; }
            const int tIdx = (int) p.tracks.size() - 1, gen0 = projGen;
            p.fm.createPluginInstanceAsync(types[idx[r - 1]], p.sr, 4096, [this, tIdx, gen0](std::unique_ptr<AudioPluginInstance> inst, const String& err) {
                if (! inst) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Plugin", err); return; }
                if (gen0 != projGen || tIdx >= (int) p.tracks.size()) return; configurePlugin(*inst, p.sr); const String nm = inst->getName();
                { ScopedLock l(p.lock); auto& t2 = *p.tracks[(size_t) tIdx]; t2.name = nm; t2.plugin = std::move(inst); } refreshAll(); });
            refreshAll(); });
    }
    void setTitle() { if (auto* w = getTopLevelComponent()) w->setName(startOn ? String("MiniDAW") : "MiniDAW - " + (projFile != File() ? projFile.getFileName() : String("Untitled")) + (dirty ? " (edited)" : "")); }
    void saveProject(bool saveAs, std::function<void()> then = {}) {
        if (! saveAs && projFile != File() && projFile.hasFileExtension(projExt)) { if (writeProject(projFile) && then) then(); return; }      
        projDir.createDirectory();
        chooser = std::make_unique<FileChooser>("Save project", projFile != File() ? projFile.withFileExtension(projExt) : projDir.getChildFile("Untitled." + projExt), "*." + projExt);
        chooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting, [this, then](const FileChooser& fc) {
            auto f = fc.getResult(); if (f == File()) return; if (! f.hasFileExtension(projExt)) f = f.getParentDirectory().getChildFile(f.getFileName() + "." + projExt); if (writeProject(f) && then) then(); });
    }
    bool writeProject(const File& f, bool makeCurrent = true, bool quiet = false) {
        XmlElement root("MINIDAW"); std::vector<std::shared_ptr<std::vector<float>>> blobs;
        { ScopedLock l(p.lock); root.setAttribute("bpm", (double) p.bpm); root.setAttribute("length", p.length); root.setAttribute("bpb", p.beatsPerBar.load()); saveMaster(root);
            for (auto& mk : p.markers) { auto* me = root.createNewChildElement("MARKER"); me->setAttribute("pos", mk.first); me->setAttribute("name", mk.second); }
            root.createNewChildElement("NOTES")->addTextElement(p.notes);
            root.setAttribute("revRoom", (double) p.revRoom.load()); root.setAttribute("revDamp", (double) p.revDamp.load()); root.setAttribute("revReturn", (double) p.revReturn.load());
            for (auto& tp : p.tracks) { auto& t = *tp; auto* e = root.createNewChildElement("TRACK");
                e->setAttribute("name", t.name); e->setAttribute("audio", t.audio ? 1 : 0); e->setAttribute("preset", t.preset); e->setAttribute("vol", (double) t.vol); e->setAttribute("pan", (double) t.pan);
                e->setAttribute("mute", t.mute ? 1 : 0); e->setAttribute("solo", t.solo ? 1 : 0); e->setAttribute("monitor", t.monitor ? 1 : 0); e->setAttribute("inCh", t.inCh);
                e->setAttribute("colour", (int) t.colour.getARGB()); e->setAttribute("eqOn", t.eqOn ? 1 : 0); e->setAttribute("icon", t.icon); e->setAttribute("send", (double) t.send);
                t.eqSave(*e); e->setAttribute("cutoff", (double) t.cutoff); e->setAttribute("atk", (double) t.atk); e->setAttribute("rel", (double) t.rel);
                for (int s = -1; s < 4; ++s) { auto* pl = s < 0 ? t.plugin.get() : t.fx[s].get(); if (! pl) continue;
                    auto* pe = e->createNewChildElement("SLOT"); pe->setAttribute("slot", s); if (auto d = pl->getPluginDescription().createXml()) pe->addChildElement(d.release());
                    MemoryBlock mb; pl->getStateInformation(mb); pe->setAttribute("state", mb.toBase64Encoding()); }
                if (t.ampSim) { auto* ae = e->createNewChildElement("AMPSIM"); ae->setAttribute("slot", t.ampSimSlot); ampsim_io::toXml(*t.ampSim, *ae); }
                for (auto& c : t.clips) { if (c.rec) continue; auto* ce = e->createNewChildElement("CLIP"); ce->setAttribute("start", c.start); ce->setAttribute("len", c.len); ce->setAttribute("mute", c.mute ? 1 : 0); ce->setAttribute("fadeIn", c.fadeIn); ce->setAttribute("fadeOut", c.fadeOut);
                    if (c.wav) { auto bid = [&blobs](const std::shared_ptr<std::vector<float>>& w) { for (size_t i = 0; i < blobs.size(); ++i) if (blobs[i] == w) return (int) i; blobs.push_back(w); return (int) blobs.size() - 1; };
                        ce->setAttribute("blob", bid(c.wav)); if (c.wavR) ce->setAttribute("blobR", bid(c.wavR)); ce->setAttribute("sr", c.wavSr); ce->setAttribute("offset", c.offset); }
                    for (auto& n : c.notes) { auto* ne = ce->createNewChildElement("N"); ne->setAttribute("s", n.start); ne->setAttribute("l", n.len); ne->setAttribute("p", n.pitch); ne->setAttribute("v", n.vel); } } } }
        TemporaryFile tmpf(f); int64 expected = 0; bool wrote = false;
        { FileOutputStream fo(tmpf.getFile());
          if (fo.openedOk()) {
              fo.write(kMagic3, 8); const String xs = root.toString(); MemoryOutputStream mo; { GZIPCompressorOutputStream gz(mo, 6); gz.write(xs.toRawUTF8(), xs.getNumBytesAsUTF8()); }
              fo.writeInt((int) mo.getDataSize()); fo.write(mo.getData(), mo.getDataSize()); fo.writeInt((int) blobs.size());
              for (auto& bl : blobs) { fo.writeInt64((int64) bl->size()); fo.write(bl->data(), bl->size() * sizeof(float)); }
              fo.flush(); expected = fo.getPosition(); wrote = ! fo.getStatus().failed(); } }
        if (! wrote || tmpf.getFile().getSize() != expected || ! tmpf.overwriteTargetFileWithTemporary()) {
            if (! quiet) AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Save", "Could not save " + f.getFullPathName() + ". Check free disk space and permissions."); return false; }
        if (makeCurrent) { projFile = f; addRecent(f); markClean(); } return true;
    }
    void openProject() {
        projDir.createDirectory(); chooser = std::make_unique<FileChooser>("Open project", projDir, "*.mdp;*.minidaw");
        chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this](const FileChooser& fc) { auto f = fc.getResult(); if (f != File()) readProject(f); });
    }
    static void configurePlugin(AudioPluginInstance& pl, double rate) {   // force a stereo layout, then prepare (done off the audio lock)
        AudioProcessor::BusesLayout lay; const auto st = AudioChannelSet::stereo();
        for (int i = 0; i < pl.getBusCount(true); ++i) lay.inputBuses.add(i == 0 ? st : AudioChannelSet::disabled());
        for (int i = 0; i < pl.getBusCount(false); ++i) lay.outputBuses.add(i == 0 ? st : AudioChannelSet::disabled());
        if (pl.checkBusesLayoutSupported(lay)) pl.setBusesLayout(lay);
        pl.setRateAndBufferSizeDetails(rate, 4096); pl.prepareToPlay(rate, 4096); }
    void loadPluginInto(int ti, int slot, const XmlElement& pe) {                      
        auto* dx = pe.getChildByName("PLUGIN"); PluginDescription d; if (dx == nullptr || ! d.loadFromXml(*dx)) return;
        MemoryBlock mb; mb.fromBase64Encoding(pe.getStringAttribute("state")); const int gen = projGen;
        p.fm.createPluginInstanceAsync(d, p.sr, 4096, [this, gen, ti, slot, mb](std::unique_ptr<AudioPluginInstance> inst, const String&) {
            if (! inst || gen != projGen || ti >= (int) p.tracks.size()) return;
            configurePlugin(*inst, p.sr); inst->setStateInformation(mb.getData(), (int) mb.getSize());
            { ScopedLock l(p.lock); auto& t = ti < 0 ? p.masterTrack : *p.tracks[(size_t) ti]; (slot < 0 ? t.plugin : t.fx[slot]) = std::move(inst); } refreshAll(); });
    }
    static bool isProjectFile(const File& f) { const String e = f.getFileExtension().toLowerCase(); return e == ".mdp" || e == ".minidaw"; }
    void openExternal(const File& f) { if (f.existsAsFile()) confirmDiscard([this, f] { readProject(f); }); }      
    void readProject(const File& f) {
        std::unique_ptr<XmlElement> xml; std::vector<std::shared_ptr<std::vector<float>>> blobs;
        { FileInputStream fi(f); if (! fi.openedOk()) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Open", "Could not open " + f.getFileName()); return; }
            char mg[8] {}; fi.read(mg, 8); fi.setPosition(0); bool newFmt = false, damaged = false;
            if (std::memcmp(mg, kMagic3, 8) == 0) {
                fi.setPosition(8); const int cl = jmax(0, fi.readInt()); MemoryBlock cx; cx.setSize((size_t) cl); fi.read(cx.getData(), cl);
                for (auto fmt : { GZIPDecompressorInputStream::zlibFormat, GZIPDecompressorInputStream::gzipFormat, GZIPDecompressorInputStream::deflateFormat }) {
                    MemoryInputStream mis(cx, false); GZIPDecompressorInputStream gzs(&mis, false, fmt); xml = parseXML(gzs.readEntireStreamAsString()); if (xml != nullptr) break; }
                const int nb = jmax(0, fi.readInt());
                for (int i = 0; i < nb; ++i) { const int64 n = fi.readInt64(); const int64 left = fi.getTotalLength() - fi.getPosition();
                    if (n < 0 || n * 4 > left) { damaged = true; blobs.push_back(std::make_shared<std::vector<float>>()); break; }
                    auto v = std::make_shared<std::vector<float>>((size_t) n); if (n > 0 && fi.read(v->data(), (int) (n * 4)) != (int) (n * 4)) damaged = true; blobs.push_back(v); }
                newFmt = true; if (damaged) AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Open", "Some audio data in this project is damaged or missing."); }
            const char first = (char) fi.readByte(); fi.setPosition(0);
            if (newFmt) {} else if (first == '<') xml = parseXML(f);                                       
            else { bool ok = false;                                                                 
                for (auto fmt : { GZIPDecompressorInputStream::gzipFormat, GZIPDecompressorInputStream::zlibFormat, GZIPDecompressorInputStream::deflateFormat }) {
                    fi.setPosition(0); GZIPDecompressorInputStream gz(&fi, false, fmt);
                    if (gz.readString() != kMagic) continue;
                    const int xl = jmax(0, gz.readInt()); MemoryBlock mb; mb.setSize((size_t) xl); gz.read(mb.getData(), xl); xml = parseXML(mb.toString());
                    const int nb = jmax(0, gz.readInt());
                    for (int i = 0; i < nb; ++i) { const int n = jmax(0, gz.readInt()); auto v = std::make_shared<std::vector<float>>((size_t) n); gz.read(v->data(), n * (int) sizeof(float)); blobs.push_back(v); }
                    ok = true; break; }
                if (! ok) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Open", "This is not a MiniDAW project (unrecognised file)."); return; } } }
        if (! xml || xml->getTagName() != "MINIDAW") { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Open", "This is not a MiniDAW project."); return; }
        setPlaying(false); pwin.reset(); ++projGen;
        { ScopedLock l(p.lock); p.tracks.clear(); p.undoS.clear(); p.redoS.clear(); p.bpm = xml->getDoubleAttribute("bpm", 120); p.length = jmax(128.0, xml->getDoubleAttribute("length", 128));
            p.beatsPerBar = jlimit(1, 16, xml->getIntAttribute("bpb", 4)); resetMaster();
            if (auto* me = xml->getChildByName("MASTER")) { p.master = (float) me->getDoubleAttribute("vol", 0.9); p.masterTrack.pan = (float) me->getDoubleAttribute("pan", 0); p.masterTrack.mute = me->getIntAttribute("mute") != 0; p.masterTrack.eqOn = me->getIntAttribute("eqOn", 1) != 0;
                p.masterTrack.eqLoad(*me); p.masterLimit = me->getIntAttribute("limit") != 0; }
            p.markers.clear(); for (auto* me : xml->getChildWithTagNameIterator("MARKER")) p.markers.push_back({ me->getDoubleAttribute("pos"), me->getStringAttribute("name") });
            p.notes = xml->getChildByName("NOTES") != nullptr ? xml->getChildByName("NOTES")->getAllSubText() : String();
            p.revRoom = (float) xml->getDoubleAttribute("revRoom", 0.6); p.revDamp = (float) xml->getDoubleAttribute("revDamp", 0.5); p.revReturn = (float) xml->getDoubleAttribute("revReturn", 0.35);
            for (auto* e : xml->getChildWithTagNameIterator("TRACK")) { auto t = std::make_unique<Track>(); t->audio = e->getIntAttribute("audio") != 0;
                Project::applyPreset(*t, jlimit(0, Project::kPresets - 1, e->getIntAttribute("preset"))); t->name = e->getStringAttribute("name");
                t->cutoff = (float) e->getDoubleAttribute("cutoff", t->cutoff); t->atk = (float) e->getDoubleAttribute("atk", t->atk); t->rel = (float) e->getDoubleAttribute("rel", t->rel);
                t->vol = (float) e->getDoubleAttribute("vol", 0.8); t->pan = (float) e->getDoubleAttribute("pan", 0); t->mute = e->getIntAttribute("mute") != 0; t->solo = e->getIntAttribute("solo") != 0;
                t->monitor = e->getIntAttribute("monitor") != 0; t->inCh = e->getIntAttribute("inCh"); t->colour = Colour((uint32) e->getIntAttribute("colour")); t->eqOn = e->getIntAttribute("eqOn", 1) != 0; t->icon = e->getIntAttribute("icon", 0); t->send = (float) e->getDoubleAttribute("send", 0.0);
                t->eqLoad(*e);
                for (auto* ce : e->getChildWithTagNameIterator("CLIP")) { Clip c(ce->getDoubleAttribute("start"), ce->getDoubleAttribute("len")); c.mute = ce->getIntAttribute("mute") != 0; c.fadeIn = ce->getDoubleAttribute("fadeIn"); c.fadeOut = ce->getDoubleAttribute("fadeOut");
                    const int bi = ce->getIntAttribute("blob", -1);
                    if (bi >= 0 && bi < (int) blobs.size()) { c.wav = blobs[(size_t) bi]; const int bR = ce->getIntAttribute("blobR", -1);
                        if (bR >= 0 && bR < (int) blobs.size() && blobs[(size_t) bR]->size() == c.wav->size()) c.wavR = blobs[(size_t) bR];
                        c.wavSr = ce->getDoubleAttribute("sr", 44100); c.offset = jmax(0.0, ce->getDoubleAttribute("offset", 0.0)); rebuildPeaks(c); }
                    for (auto* ne : ce->getChildWithTagNameIterator("N")) c.notes.push_back({ ne->getDoubleAttribute("s"), ne->getDoubleAttribute("l"), ne->getIntAttribute("p"), jlimit(1, 127, ne->getIntAttribute("v", 100)) });
                    t->clips.push_back(c); }
                if (auto* ae = e->getChildByName("AMPSIM")) {
                    auto ap = std::make_unique<ampsim::AmpSimProcessor>(); ampsim_io::fromXml(*ap, *ae); ap->prepare(p.sr);
                    t->ampSimSlot = jlimit(0, 3, ae->getIntAttribute("slot", 0)); t->ampSim = std::move(ap); }
                t->coll.reset(p.sr); p.tracks.push_back(std::move(t)); }
            p.selTrack = p.tracks.empty() ? -1 : 0; p.selClip = -1; p.beat = 0; }
        projFile = f; tempo.setValue(p.bpm, dontSendNotification); int ti = 0;
        for (auto* e : xml->getChildWithTagNameIterator("TRACK")) { for (auto* pe : e->getChildWithTagNameIterator("SLOT")) loadPluginInto(ti, pe->getIntAttribute("slot"), *pe); ++ti; }
        if (auto* me = xml->getChildByName("MASTER")) for (auto* pe : me->getChildWithTagNameIterator("SLOT")) loadPluginInto(-1, pe->getIntAttribute("slot"), *pe);
        refreshAll(); addRecent(f); markClean(); hideStart();
    }
    // ---- export / bounce / MIDI ----
    ExportOpts expo; static constexpr double kRates[4] = { 44100.0, 48000.0, 88200.0, 96000.0 }; static constexpr double kTails[5] = { 0.0, 1.0, 2.0, 4.0, 8.0 };
    String defaultBase() const { return projFile != File() ? projFile.getFileNameWithoutExtension() : String("MiniDAW"); }
    void exportDialog(int preset = 0) {
        const bool hasRegion = p.selected() != nullptr, hasTrack = selT() != nullptr; if (preset > 0) expo.content = preset; expo.base = defaultBase();
        auto* c = new ExportPanel(expo, lastTake != nullptr, hasRegion, hasTrack, hasTrack ? selT()->name : String());
        c->setSize(470, 500); c->onExport = [this](const ExportOpts& o) { expo = o; startExport(o); }; launch(c, "Export / Bounce");
    }
    void startExport(const ExportOpts& o) {
        const String ext = o.format == 1 ? ".wav" : o.format == 2 ? ".aif" : ".flac"; const File music = File::getSpecialLocation(File::userMusicDirectory);
        if (o.content == 3 || o.content == 4) {
            chooser = std::make_unique<FileChooser>("Choose a folder for the exported files", music);
            chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories, [this, o](const FileChooser& fc) { auto d = fc.getResult(); if (d != File()) beginExport(o, d, File()); });
            return; }
        String nm = o.base; if ((o.content == 5 || o.content == 6) && selT() != nullptr) nm += " - " + selT()->name + (o.content == 5 ? " (region)" : String());
        chooser = std::make_unique<FileChooser>("Export audio", music.getChildFile(File::createLegalFileName(nm) + ext), "*" + ext);
        chooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
            [this, o, ext](const FileChooser& fc) { auto f = fc.getResult(); if (f != File()) beginExport(o, f.getParentDirectory(), f.withFileExtension(ext)); });
    }
    void beginExport(const ExportOpts& o, const File& dir, const File& single) {
        auto st = std::make_shared<ExportState>(); st->o = o; st->dir = dir; st->single = single; st->ext = o.format == 1 ? ".wav" : o.format == 2 ? ".aif" : ".flac"; st->tail = kTails[jlimit(0, 4, o.tail)];
        { ScopedLock l(p.lock);
          for (auto& t : p.tracks) for (auto& c : t->clips) st->b1 = jmax(st->b1, c.start + c.len);
          if (o.range == 1 && p.loopEnd > p.loopStart) { st->b0 = p.loopStart; st->b1 = p.loopEnd; }
          if (auto* sc = p.selected()) if (o.content == 5 || o.range == 2) { st->b0 = sc->start; st->b1 = sc->start + sc->len; }
          if (o.content == 1) st->jobs.push_back({ String(), -2 });
          else if (o.content == 2) st->jobs.push_back({ String(), -1 });
          else if (o.content == 5 || o.content == 6) st->jobs.push_back({ String(), p.selTrack });
          else { if (o.content == 4) st->jobs.push_back({ o.base + " - Mixdown", -1 });
                 for (size_t i = 0; i < p.tracks.size(); ++i) { if (o.skipMuted && p.tracks[i]->mute) continue; st->jobs.push_back({ o.base + " - " + String((int) i + 1).paddedLeft('0', 2) + " " + p.tracks[i]->name, (int) i }); } } }
        if (st->jobs.empty()) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Export", "Nothing to export."); return; }
        auto* ep = new ExportProgress(); st->prog = ep; DialogWindow::LaunchOptions lo; lo.content.setOwned(ep); lo.dialogTitle = "Exporting..."; lo.componentToCentreAround = this;
        lo.dialogBackgroundColour = Colour(0xff25262b); lo.useNativeTitleBar = false; lo.resizable = false; lo.escapeKeyTriggersCloseButton = false;
        if (auto* w = lo.launchAsync()) { w->setTitleBarHeight(36); st->win = w; }
        Timer::callAfterDelay(80, [this, st] { stepExport(st); });
    }
    void stepExport(std::shared_ptr<ExportState> st) {
        if (st->prog == nullptr) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Export cancelled", String(st->written.size()) + " file(s) were written before cancelling."); return; }
        if (st->idx >= st->jobs.size()) { finishExport(st); return; }
        auto& jb = st->jobs[st->idx];
        if (! st->rendering) { st->prog->set((double) st->idx / (double) st->jobs.size(), "Rendering " + String((int) st->idx + 1) + " of " + String((int) st->jobs.size()) + (jb.name.isNotEmpty() ? ":  " + jb.name : String()));
            st->rendering = true; Timer::callAfterDelay(30, [this, st] { stepExport(st); }); return; }
        st->rendering = false;
        const File f = st->single != File() ? st->single : st->dir.getChildFile(File::createLegalFileName(jb.name) + st->ext);
        AudioBuffer<float> buf;
        if (jb.solo == -2) { if (lastTake) { const int n = (int) lastTake->size(); buf.setSize(2, n); for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, lastTake->data(), n); } }
        else buf = renderRange(st->b0, st->b1, st->tail, jb.solo);
        if (buf.getNumSamples() < 2) st->errors.add("Nothing to render for " + f.getFileName());
        else { const String err = writeBuffer(buf, p.sr, f, st->o); if (err.isEmpty()) st->written.add(f.getFullPathName()); else st->errors.add(err); }
        ++st->idx; Timer::callAfterDelay(30, [this, st] { stepExport(st); });
    }
    void finishExport(std::shared_ptr<ExportState> st) {
        if (st->win != nullptr) st->win->exitModalState(0);
        String msg; if (! st->written.isEmpty()) { msg << "Exported " << st->written.size() << (st->written.size() == 1 ? " file" : " files") << ":\n" << st->written.joinIntoString("\n").substring(0, 700); }
        if (! st->errors.isEmpty()) msg << (msg.isEmpty() ? String() : String("\n\n")) << "Problems:\n" << st->errors.joinIntoString("\n");
        if (msg.isEmpty()) msg = "Nothing was exported.";
        AlertWindow::showMessageBoxAsync(st->errors.isEmpty() ? MessageBoxIconType::InfoIcon : MessageBoxIconType::WarningIcon, "Export finished", msg);
        if (st->o.reveal && ! st->written.isEmpty()) File(st->written[0]).revealToUser();
    }
    AudioBuffer<float> renderRange(double b0, double b1, double tailSec, int soloTrack) {
        ScopedLock l(p.lock); const bool wasPlaying = p.playing, wasCycle = p.cycle, wasMetro = p.metro; const double beat0 = p.beat, sr = p.sr;
        std::vector<char> mu, so; for (auto& t : p.tracks) { mu.push_back((char) (t->mute ? 1 : 0)); so.push_back((char) (t->solo ? 1 : 0)); }
        if (soloTrack >= 0) for (size_t i = 0; i < p.tracks.size(); ++i) { p.tracks[i]->solo = (int) i == soloTrack; if ((int) i == soloTrack) p.tracks[i]->mute = false; }
        for (auto& t : p.tracks) { if (t->plugin) t->plugin->reset(); for (auto& f : t->fx) if (f) f->reset(); for (auto& bd : t->bq) bd.reset(); }
        for (auto& f : p.masterTrack.fx) if (f) f->reset();
        const int total = jmax(2, (int) (((b1 - b0) * 60.0 / p.bpm + tailSec) * sr)), blk = 512;
        AudioBuffer<float> out(2, total), in(2, blk), mixb(2, blk); in.clear(); Engine e(p);
        p.beat = b0; p.playing = true; p.cycle = false; p.metro = false;
        for (int pos = 0; pos < total; pos += blk) { const int n = jmin(blk, total - pos);
            AudioBuffer<float> view(mixb.getArrayOfWritePointers(), 2, n), inv(in.getArrayOfWritePointers(), 2, n); e.render(view, n, sr, inv);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, pos, view, ch, 0, n); }
        for (size_t i = 0; i < p.tracks.size(); ++i) { p.tracks[i]->mute = mu[i] != 0; p.tracks[i]->solo = so[i] != 0; }
        p.playing = wasPlaying; p.cycle = wasCycle; p.metro = wasMetro; p.beat = beat0; return out;
    }
    static AudioBuffer<float> resampleBuffer(const AudioBuffer<float>& in, double srIn, double srOut) {
        const double ratio = srIn / srOut; const int nIn = in.getNumSamples(), nOut = jmax(1, (int) std::ceil((double) nIn / ratio)); AudioBuffer<float> out(in.getNumChannels(), nOut);
        std::vector<float> pad((size_t) nIn + 16, 0.f);
        for (int ch = 0; ch < in.getNumChannels(); ++ch) { std::copy(in.getReadPointer(ch), in.getReadPointer(ch) + nIn, pad.begin()); LagrangeInterpolator li; li.process(ratio, pad.data(), out.getWritePointer(ch), nOut); }
        return out; }
    String writeBuffer(AudioBuffer<float> buf, double sr, const File& f, const ExportOpts& o) {
        int n = buf.getNumSamples();
        if (o.channels == 1 && buf.getNumChannels() == 2) { AudioBuffer<float> m(1, n); m.copyFrom(0, 0, buf, 0, 0, n); m.addFrom(0, 0, buf, 1, 0, n); m.applyGain(0.5f); buf = std::move(m); }
        if (o.trim) { int last = n - 1; while (last > 0) { bool loud = false; for (int ch = 0; ch < buf.getNumChannels(); ++ch) if (std::abs(buf.getSample(ch, last)) > 1.0e-4f) { loud = true; break; } if (loud) break; --last; }
            n = jmax(2, jmin(n, last + 1 + (int) (0.05 * sr))); buf.setSize(buf.getNumChannels(), n, true); }
        if (o.norm > 0) { const float pk = buf.getMagnitude(0, buf.getNumSamples()); if (pk > 0.f) buf.applyGain(core::normGain(pk, o.norm == 1 ? -1.f : -0.1f)); }
        const double outSr = o.rate == 0 ? sr : kRates[jlimit(0, 3, o.rate - 1)];
        if (std::abs(outSr - sr) > 1.0) buf = resampleBuffer(buf, sr, outSr);
        if (o.depth == 1 && o.dither) { Random rng; for (int ch = 0; ch < buf.getNumChannels(); ++ch) { float* d = buf.getWritePointer(ch); for (int i = 0; i < buf.getNumSamples(); ++i) d[i] = jlimit(-1.f, 1.f, d[i] + (rng.nextFloat() - rng.nextFloat()) * (1.f / 32768.f)); } }
        std::unique_ptr<AudioFormat> af; if (o.format == 1) af = std::make_unique<WavAudioFormat>(); else if (o.format == 2) af = std::make_unique<AiffAudioFormat>(); else af = std::make_unique<FlacAudioFormat>();
        int bits = o.depth == 1 ? 16 : o.depth == 2 ? 24 : 32; if (bits == 32 && o.format != 1) bits = 24;
        f.deleteFile(); auto fs = std::make_unique<FileOutputStream>(f); if (! fs->openedOk()) return "Could not write " + f.getFullPathName();
        std::unique_ptr<AudioFormatWriter> w(af->createWriterFor(fs.get(), outSr, (unsigned int) buf.getNumChannels(), bits, {}, 0));
        if (! w) return "This format / bit depth / sample rate combination is not supported (" + f.getFileName() + ").";
        fs.release(); w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()); w.reset(); return {};
    }
    void addAudioTrackWithClip(const String& name, Colour col, std::shared_ptr<std::vector<float>> wav, double sr, double startBeat) {
        p.addTrack(true);
        { ScopedLock l(p.lock); p.pushUndo(); auto& tr = *p.tracks.back(); tr.name = name; tr.colour = col; const int n = (int) wav->size();
          Clip c(startBeat, (double) n / sr * p.bpm / 60.0); c.wav = wav; c.wavSr = sr; core::updatePeaks(wav->data(), n, c.peaksDone, c.peaks); core::finishPeaks(wav->data(), n, c.peaksDone, c.peaks); tr.clips.push_back(std::move(c));
          p.selTrack = (int) p.tracks.size() - 1; p.selClip = -1; }
        refreshAll(); arr.repaint(); }
    void bounceInPlace() {
        auto* t = selT(); if (t == nullptr) return; const int ti = p.selTrack; const String nm = t->name; const Colour col = t->colour; double e = 4.0;
        { ScopedLock l(p.lock); for (auto& tp : p.tracks) for (auto& c : tp->clips) e = jmax(e, c.start + c.len); }
        auto buf = renderRange(0.0, e, 2.0, ti); const int n = buf.getNumSamples(); auto wav = std::make_shared<std::vector<float>>((size_t) n);
        for (int i = 0; i < n; ++i) (*wav)[(size_t) i] = 0.5f * (buf.getSample(0, i) + buf.getSample(1, i));
        addAudioTrackWithClip(nm + " (bounce)", col, wav, p.sr, 0.0);
        { ScopedLock l(p.lock); if (ti >= 0 && ti < (int) p.tracks.size()) p.tracks[(size_t) ti]->mute = true; }
    }
    void exportMidi(bool allTracks) {
        const File music = File::getSpecialLocation(File::userMusicDirectory); String nm = defaultBase(); if (! allTracks && selT() != nullptr) nm += " - " + selT()->name;
        chooser = std::make_unique<FileChooser>("Export MIDI file", music.getChildFile(File::createLegalFileName(nm) + ".mid"), "*.mid");
        chooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting, [this, allTracks](const FileChooser& fc) { auto f = fc.getResult(); if (f != File()) writeMidi(f.withFileExtension(".mid"), allTracks); });
    }
    void writeMidi(const File& f, bool allTracks) {
        MidiFile mf; mf.setTicksPerQuarterNote(960); const double tpq = 960.0; int count = 0;
        { ScopedLock l(p.lock);
          { MidiMessageSequence cond; cond.addEvent(MidiMessage::tempoMetaEvent((int) (60000000.0 / p.bpm)), 0.0); cond.addEvent(MidiMessage::timeSignatureMetaEvent(jmax(1, p.beatsPerBar.load()), 4), 0.0); mf.addTrack(cond); }
          for (size_t ti = 0; ti < p.tracks.size(); ++ti) { auto& t = *p.tracks[ti]; if (t.audio) continue; if (! allTracks && (int) ti != p.selTrack) continue;
              MidiMessageSequence seq; seq.addEvent(MidiMessage::textMetaEvent(3, t.name), 0.0);
              for (auto& c : t.clips) { if (c.mute) continue; for (auto& nt : c.notes) { const double s0 = (c.start + nt.start) * tpq, s1 = (c.start + nt.start + nt.len) * tpq;
                  seq.addEvent(MidiMessage::noteOn(1, nt.pitch, (juce::uint8) jlimit(1, 127, nt.vel)), s0); seq.addEvent(MidiMessage::noteOff(1, nt.pitch), s1); } }
              seq.updateMatchedPairs(); mf.addTrack(seq); ++count; } }
        if (count == 0) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Export MIDI", "There are no instrument tracks to export."); return; }
        f.deleteFile(); FileOutputStream fs(f); if (! fs.openedOk() || ! mf.writeTo(fs)) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Export MIDI", "Could not write " + f.getFullPathName()); return; }
        fs.flush(); AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Export finished", "Saved " + f.getFullPathName());
    }
    void importMidi() {
        chooser = std::make_unique<FileChooser>("Import MIDI file", File::getSpecialLocation(File::userMusicDirectory), "*.mid;*.midi");
        chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this](const FileChooser& fc) { auto f = fc.getResult(); if (f != File()) readMidi(f); });
    }
    void readMidi(const File& f) {
        FileInputStream in(f); MidiFile mf; if (! in.openedOk() || ! mf.readFrom(in)) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Import MIDI", "Could not read " + f.getFileName()); return; }
        const int tf = mf.getTimeFormat(); if (tf <= 0) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Import MIDI", "SMPTE time-based MIDI files are not supported."); return; }
        const double ppq = (double) tf; int made = 0; double newBpm = 0.0; bool emptyProject = true; { ScopedLock l(p.lock); for (auto& t : p.tracks) if (! t->clips.empty()) emptyProject = false; }
        const int bpb = jmax(1, p.beatsPerBar.load());
        for (int ti = 0; ti < mf.getNumTracks(); ++ti) {
            MidiMessageSequence seq(*mf.getTrack(ti)); seq.updateMatchedPairs(); String nm; std::vector<Note> notes;
            for (int i = 0; i < seq.getNumEvents(); ++i) { auto* ev = seq.getEventPointer(i); const auto& m = ev->message;
                if (m.isTempoMetaEvent()) { if (newBpm <= 0.0) newBpm = 60.0 / m.getTempoSecondsPerQuarterNote(); }
                else if (m.isTrackNameEvent()) { if (nm.isEmpty()) nm = m.getTextFromTextMetaEvent(); }
                else if (m.isNoteOn() && ev->noteOffObject != nullptr) { const double s0 = m.getTimeStamp() / ppq, l0 = jmax(0.05, (ev->noteOffObject->message.getTimeStamp() - m.getTimeStamp()) / ppq);
                    notes.push_back({ s0, l0, m.getNoteNumber(), jlimit(1, 127, (int) m.getVelocity()) }); } }
            if (notes.empty()) continue;
            p.addTrack(false);
            { ScopedLock l(p.lock); auto& t = *p.tracks.back(); t.name = nm.isNotEmpty() ? nm : "MIDI " + String(made + 1); double end = 0.0; for (auto& n : notes) end = jmax(end, n.start + n.len);
              Clip c(0.0, jmax((double) bpb, std::ceil(end / bpb) * bpb)); c.notes = notes; p.length = jmax(p.length, c.len + 32.0); t.clips.push_back(std::move(c)); p.selTrack = (int) p.tracks.size() - 1; p.selClip = -1; }
            ++made; }
        if (made == 0) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Import MIDI", "No notes were found in " + f.getFileName()); return; }
        if (emptyProject && newBpm >= 20.0 && newBpm <= 300.0) { p.bpm = newBpm; tempo.setValue(p.bpm, dontSendNotification); }
        refreshAll(); arr.repaint();
    }
    void deleteTrack(int i) {
        if (i < 0 || i >= (int) p.tracks.size()) return;
        if (p.recClip >= 0) setPlaying(false);
        pwin.reset(); arr.dragT = -1;
        { ScopedLock l(p.lock); p.undoS.clear(); p.redoS.clear(); p.tracks.erase(p.tracks.begin() + i); p.selClip = -1; p.recTrack = p.recClip = -1; p.selTrack = jmin(i, (int) p.tracks.size() - 1); }
        refreshAll();
    }
    void openAmpSim(Track& t) {
        pwin.reset(); if (! t.ampSim) return;
        pwin = std::make_unique<PluginWindow>(new AmpSimPanel(*t.ampSim, p.lock, t.name), "Amp Sim - " + t.name);
    }
    void openEditor(AudioPluginInstance* pl) {
        pwin.reset(); if (! pl) return;
        auto* e = pl->hasEditor() ? pl->createEditorIfNeeded() : new GenericAudioProcessorEditor(*pl);
        pwin = std::make_unique<PluginWindow>(e, pl->getName());
    }
    void slotMenu(Track& t, int slot) {                              
        auto types = p.known.getTypes(); std::vector<PickItem> items; Array<int> idx; auto& cur = slot < 0 ? t.plugin : t.fx[slot]; const bool isIn = slot < 0 && t.audio;
        if (isIn) for (int c = 0; c < 8; ++c) { PickItem it; it.id = 6000 + c; it.name = "Input " + String(c + 1); it.sub = "Audio interface input"; it.badge = "Input"; it.ticked = t.inCh == c; it.chip = 1; items.push_back(it); }
        if (slot < 0 && ! t.audio) for (int i = 0; i < Project::kPresets; ++i) { PickItem it; it.id = 5000 + i; it.name = Project::presets[i].name; it.sub = "Built-in instrument"; it.badge = "Built-in"; it.ticked = ! t.plugin && t.preset == i; it.chip = 1; items.push_back(it); }
        if (! isIn) { std::vector<int> order; for (int i = 0; i < types.size(); ++i) if (types[i].isInstrument == (slot < 0)) order.push_back(i);
            std::sort(order.begin(), order.end(), [&types](int a, int b) { return types[a].name.compareIgnoreCase(types[b].name) < 0; });
            for (int i : order) { PickItem it; it.id = idx.size() + 1; it.name = types[i].name; it.sub = types[i].manufacturerName; it.badge = types[i].pluginFormatName; it.chip = 2; items.push_back(it); idx.add(i); } }
        const bool ampOk = slot >= 0 && &t != &p.masterTrack, ampHere = ampOk && t.hasAmp(slot);
        std::vector<std::pair<String, int>> acts; if (cur || ampHere) { acts.push_back({ "Open Editor", 9001 }); acts.push_back({ "Remove", 9000 }); } acts.push_back({ "Scan for Plug-ins...", 9002 });
        const StringArray chips = isIn ? StringArray() : slot < 0 ? StringArray { "All", "Built-in", "Plug-ins" } : StringArray { "All", "Plug-ins" };
        if (slot >= 0) for (auto& it : items) it.chip = 1;
        if (ampOk) { PickItem it; it.id = 7000; it.name = "Amp Sim"; it.sub = "Amp, cabinet and pedalboard"; it.badge = "Built-in"; it.ticked = ampHere; it.chip = 1; items.insert(items.begin(), it); }
        showPicker(isIn ? "Input" : slot < 0 ? "Instrument" : "Insert " + String(slot + 1), std::move(items), chips, std::move(acts), [this, &t, &cur, slot, types, idx](int r) {
            if (r == 0) return; if (r == 9002) { scan.triggerClick(); return; } if (r == 9001) { if (slot >= 0 && t.hasAmp(slot)) openAmpSim(t); else openEditor(cur.get()); return; }
            pwin.reset();
            if (r == 9000) { { ScopedLock l(p.lock); cur.reset(); if (slot >= 0 && t.hasAmp(slot)) { t.ampSim.reset(); t.ampSimSlot = -1; } } refreshAll(); return; }
            if (r == 7000) {                                                    // built-in amp sim (one per track; picking it again moves it here)
                { ScopedLock l(p.lock); if (! t.ampSim) { t.ampSim = std::make_unique<ampsim::AmpSimProcessor>(); t.ampSim->prepare(p.sr); } t.fx[slot].reset(); t.ampSimSlot = slot; }
                refreshAll(); openAmpSim(t); return; }
            if (r >= 6000) { ScopedLock l(p.lock); t.inCh = r - 6000; refreshAll(); return; }
            if (r >= 5000) { ScopedLock l(p.lock); t.plugin.reset(); Project::applyPreset(t, r - 5000); refreshAll(); return; }
            int tiB = -1; for (size_t q = 0; q < p.tracks.size(); ++q) if (p.tracks[q].get() == &t) tiB = (int) q; const int genB = projGen;
            p.fm.createPluginInstanceAsync(types[idx[r - 1]], p.sr, 4096, [this, tiB, genB, slot](std::unique_ptr<AudioPluginInstance> inst, const String& err) {
                if (! inst) { AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Plugin", err); return; }
                if (genB != projGen || tiB >= (int) p.tracks.size()) return; configurePlugin(*inst, p.sr);
                { ScopedLock l(p.lock); Track& tt = tiB < 0 ? p.masterTrack : *p.tracks[(size_t) tiB]; if (slot >= 0 && tt.hasAmp(slot)) { tt.ampSim.reset(); tt.ampSimSlot = -1; }
                  (slot < 0 ? tt.plugin : tt.fx[slot]) = std::move(inst); } refreshAll(); });
        });
    }
    void handleIncomingMidiMessage(MidiInput*, const MidiMessage& m) override {
        ScopedLock sl(p.lock); if (p.selTrack < 0 || p.selTrack >= (int) p.tracks.size()) return; auto& t = *p.tracks[(size_t) p.selTrack];
        if (t.plugin) t.coll.addMessageToQueue(m);
        const int n = m.getNoteNumber();
        if (m.isNoteOn()) { t.live[n] = true; if (p.recClip >= 0 && ! p.counting) { pend[n] = p.beat; pendV[n] = (int) m.getVelocity(); } }
        else if (m.isNoteOff()) { t.live[n] = false;
            if (pend[n] >= 0 && p.recClip >= 0 && p.beat > pend[n]) { auto& c = p.tracks[(size_t) p.recTrack]->clips[(size_t) p.recClip]; c.notes.push_back({ pend[n] - c.start, jmax(0.1, (double) p.beat - pend[n]), n, jlimit(1, 127, pendV[n]) }); }
            pend[n] = -1; }
    }
    bool keyPressed(const KeyPress& k) override {                    
        if (k == KeyPress('S', ModifierKeys::commandModifier, 0)) { doMenu(12); return true; }
        if (k == KeyPress('S', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 0)) { doMenu(15); return true; }   
        if (k == KeyPress('O', ModifierKeys::commandModifier, 0)) { doMenu(11); return true; }      
        if (startOn) { if (k == KeyPress('N', ModifierKeys::commandModifier, 0)) { start.setTab(StartScreen::NewTab); return true; } if (k == KeyPress(',', ModifierKeys::commandModifier, 0)) { prefsDialog(); return true; } return false; }   
        if (k == KeyPress('W', ModifierKeys::commandModifier, 0)) { closeProject(); return true; }      
        if (k == KeyPress::returnKey) { p.beat = 0; return true; }                                    
        const int cmd = ModifierKeys::commandModifier, shift = ModifierKeys::shiftModifier;
        if (k == KeyPress('Z', cmd, 0)) { doMenu(20); return true; }   if (k == KeyPress('Z', cmd | shift, 0)) { doMenu(21); return true; }
        if (k == KeyPress('T', cmd, 0)) { doMenu(22); return true; }   if (k == KeyPress('I', cmd, 0)) { doMenu(13); return true; }
        if (k == KeyPress('N', cmd, 0)) { doMenu(10); return true; }   if (k == KeyPress(',', cmd, 0)) { doMenu(69); return true; }
        if (k == KeyPress('E', ModifierKeys::commandModifier, 0)) { exportDialog(); return true; }   
        if (k == KeyPress(KeyPress::backspaceKey, ModifierKeys::commandModifier, 0)) { deleteTrack(p.selTrack); return true; }   
        if (k == KeyPress('B', cmd, 0)) { doMenu(470); return true; }
        if (k == KeyPress('X', cmd, 0)) { doMenu(450); return true; } if (k == KeyPress('C', cmd, 0)) { doMenu(451); return true; } if (k == KeyPress('V', cmd, 0)) { doMenu(452); return true; }
        if (k == KeyPress(KeyPress::tabKey, 0, 0)) { doMenu(457); return true; } if (k == KeyPress(KeyPress::tabKey, shift, 0)) { doMenu(458); return true; }
        if (! k.getModifiers().isAnyModifierKeyDown()) { const juce_wchar c0 = CharacterFunctions::toLowerCase(k.getTextCharacter());
            if (c0 == ',') { doMenu(490); return true; } if (c0 == '.') { doMenu(491); return true; } if (c0 == '\'') { doMenu(496); return true; } if (c0 == '[') { doMenu(497); return true; } if (c0 == ']') { doMenu(498); return true; }
            if (c0 == 'z') { doMenu(516); return true; }
            if (c0 == 'm') { if (auto* t0 = selT()) { ScopedLock l(p.lock); t0->mute = ! t0->mute; } return true; }
            if (c0 == 's') { if (auto* t0 = selT()) { ScopedLock l(p.lock); t0->solo = ! t0->solo; } return true; } }
        const juce_wchar c = CharacterFunctions::toLowerCase(k.getTextCharacter());
        if (k == KeyPress::spaceKey) setPlaying(! p.playing); else if (c == 'x') mixB.triggerClick(); else if (c == 'i') inspB.triggerClick(); else if (c == 'p') rollB.triggerClick(); else if (c == 'u') tunerB.triggerClick();
        else if (c == 'r') rec.triggerClick(); else if (c == 'k') metro.triggerClick(); else if (c == 't') tapTempo(); else if (c == 'c') cycB.triggerClick(); else if (c == '=' || c == '+') arr.ppb = jmin(400.0, arr.ppb * 1.25); else if (c == '-') arr.ppb = jmax(2.0, arr.ppb / 1.25); else return false;
        resized(); repaint(); return true;
    }
    int menuSig = -1;
    void refreshMenuStates() {      
        int sig = 0; { ScopedLock l(p.lock); auto* c = p.selected();
            sig = (startOn ? 1 : 0) | (c ? 2 : 0) | (selT() ? 4 : 0) | (p.undoS.empty() ? 8 : 0) | (p.redoS.empty() ? 16 : 0) | (c && c->wav && ! c->rec ? 32 : 0)
                | (inspOn ? 64 : 0) | (mixOn ? 128 : 0) | (rollOn ? 256 : 0) | (tunerOn ? 512 : 0) | (p.metro ? 1024 : 0) | (p.cycle ? 2048 : 0) | (recent.size() << 12) | (clipboard ? (1 << 22) : 0) | (jmin(100, (int) p.markers.size()) << 23); }
        if (sig != menuSig) { menuSig = sig; menuItemsChanged(); } }
    void timerCallback() override {
        refreshMenuStates(); if (++inTick % 10 == 0) autoInputs(); if (! outFixTried && inTick > 30) { outFixTried = true; fixOutputs(); }
        if (inTick % 15 == 0) if (auto* dv = deviceManager.getCurrentAudioDevice()) { const double r = dv->getCurrentSampleRate(); if (r > 0 && std::abs(r - p.sr) > 1.0) prepareToPlay(dv->getCurrentBufferSizeSamples(), r); }
        bool grew = false;
        { ScopedLock l(p.lock); if (p.recClip >= 0) { auto& c = p.tracks[(size_t) p.recTrack]->clips[(size_t) p.recClip];
            if (p.counting) { if (p.beat >= p.recFrom) p.counting = false; }
            else if (p.beat < c.start) setPlaying(false);
            else if (c.wav) { const int rp = p.recPos; core::updatePeaks(c.wav->data(), rp, c.peaksDone, c.peaks); c.len = jmax(0.25, (double) rp / c.wavSr * p.bpm / 60.0); 
                if (growing && p.recNext.load() == nullptr && ! p.recBusy) { if ((size_t) p.recCap == growBuf.size()) c.wav->swap(growBuf); growBuf = {}; growing = false; }   
                if (! growing && rp > p.recCap - (int) (c.wavSr * 20)) {                      
                    growBuf.assign((size_t) p.recCap * 2, 0.f); const int rp1 = p.recPos; std::memcpy(growBuf.data(), c.wav->data(), (size_t) rp1 * sizeof(float));
                    p.recNextFrom = rp1; p.recNextCap = (int) growBuf.size(); growing = true; p.recNext = growBuf.data(); } }
            else c.len = jmax(0.25, (double) p.beat - c.start);
            if (p.recClip >= 0 && c.start + c.len + 8 > p.length) { p.length = c.start + c.len + 32; grew = true; } } }
        { auto v = arrView.getViewPosition();                                                       
            const double rightBeat = jmax((double) (v.x + arrView.getViewWidth() - Arrangement::head) / arr.ppb, (double) p.beat);
            if (rightBeat + 64 > p.length) { p.length = rightBeat + 256; grew = true; } }
        if (grew) resized();
        if (p.playing && p.autoScroll) { const int px = Arrangement::head + (int) (p.beat * arr.ppb); auto v = arrView.getViewPosition();       
            if (px > v.x + arrView.getViewWidth() - 60) arrView.setViewPosition(jmax(0, px - arrView.getViewWidth() / 3), v.y);
            else if (px < v.x + Arrangement::head) arrView.setViewPosition(jmax(0, px - Arrangement::head - 40), v.y); }
        play.setToggleState(p.playing, dontSendNotification); metro.setToggleState(p.metro, dontSendNotification); autosaveTick(); if (! startOn && ++sigTick % 15 == 0) updateDirty();
        if (! master.isMouseOverOrDragging()) master.setValue(p.master, dontSendNotification);
        lcd.repaint(); repaint(0, 0, getWidth(), barH + 24);
    }
    void prepareToPlay(int, double rate) override { ScopedLock l(p.lock); p.sr = rate;
        { auto* ad = deviceManager.getCurrentAudioDevice(); nInActive = ad != nullptr ? ad->getActiveInputChannels().countNumberOfSetBits() : 0; }
        for (auto& t : p.tracks) { t->coll.reset(rate); if (t->plugin) t->plugin->prepareToPlay(rate, 4096); for (auto& f : t->fx) if (f) f->prepareToPlay(rate, 4096); if (t->ampSim) t->ampSim->prepare(rate); }
        for (auto& f : p.masterTrack.fx) if (f) f->prepareToPlay(rate, 4096); }
    void releaseResources() override {}
    void getNextAudioBlock(const AudioSourceChannelInfo& i) override {
        auto& b = *i.buffer; const int n = i.numSamples, nc = jmin(b.getNumChannels(), 8); ++cbCount; lastN = n; sampCount += i.numSamples;
        if (n > 4096 || n <= 0 || b.getNumChannels() <= 0) { i.clearActiveBufferRegion(); return; }
        const int nIn = jmin(nc, nInActive.load());
        for (int ch = 0; ch < nIn; ++ch) inBuf.copyFrom(ch, 0, b, ch, i.startSample, n);
        if (tuner.active.load() && nIn > 0) { tuner.numIn = nIn; tuner.push(inBuf.getReadPointer(jmin(tuner.inCh.load(), nIn - 1)), n); }
        AudioBuffer<float> in(inBuf.getArrayOfWritePointers(), nIn, n), out(b.getArrayOfWritePointers(), b.getNumChannels(), i.startSample, n);
        if (out.getNumChannels() >= 2) eng.render(out, n, p.sr, in);
        else { AudioBuffer<float> st(stereoTmp.getArrayOfWritePointers(), 2, n); eng.render(st, n, p.sr, in);          
               for (int s = 0; s < n && out.getNumChannels() > 0; ++s) out.setSample(0, s, 0.5f * (st.getSample(0, s) + st.getSample(1, s))); }
        if (testTone.load()) { const double inc = MathConstants<double>::twoPi * 440.0 / jmax(8000.0, p.sr);                    
            for (int s = 0; s < n; ++s) { const float v = 0.2f * (float) std::sin(tonePh); tonePh += inc; if (tonePh > MathConstants<double>::twoPi) tonePh -= MathConstants<double>::twoPi;
                for (int ch = 0; ch < out.getNumChannels(); ++ch) out.setSample(ch, s, v); } }
        outPeak = out.getNumChannels() > 0 ? out.getMagnitude(0, n) : 0.f;
    }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2b2c30)); const bool recording = p.rec && p.playing; const int y0 = menuBar ? 24 : 0;       
        g.setGradientFill(ColourGradient(recording ? Colour(0xff7a1d1d) : Colour(0xff414246), 0, (float) y0, recording ? Colour(0xff4a1212) : Colour(0xff2f3033), 0, (float) (y0 + barH), false)); g.fillRect(0, y0, getWidth(), barH);
        g.setColour(Colour(0xff111214)); g.fillRect(0, y0 + barH - 1, getWidth(), 1); g.setColour(Colour(0x14ffffff)); g.fillRect(0, y0, getWidth(), 1);
        for (auto* r : { &viewsR, &transR, &metroR, &volR }) { if (r->isEmpty()) continue; g.setColour(Colour(0x40000000)); g.fillRoundedRectangle(r->toFloat(), 8.f); g.setColour(Colour(0x12ffffff)); g.drawRoundedRectangle(r->toFloat().reduced(0.5f), 8.f, 1.f); }
    }
    void resized() override {
        auto r = getLocalBounds(); if (menuBar) menuBar->setBounds(r.removeFromTop(24)); auto top = r.removeFromTop(barH).reduced(10, 9);
        const int bw = 36, gap = 10;
        viewsR = top.removeFromLeft(bw * 4 + 8); { auto q = viewsR.reduced(4, 3); for (auto* b : { &inspB, &mixB, &rollB, &tunerB }) b->setBounds(q.removeFromLeft(bw)); }
        top.removeFromLeft(gap);
        transR = top.removeFromLeft(bw * 5 + 8); { auto q = transR.reduced(4, 3); for (auto* b : { &rew, &stop, &play, &rec, &cycB }) b->setBounds(q.removeFromLeft(bw)); }
        top.removeFromLeft(gap);
        auto right = top.removeFromRight(66 + 4 + 58);                                                                        
        addAud.setBounds(right.removeFromRight(66).withSizeKeepingCentre(66, 26)); right.removeFromRight(4); addInst.setBounds(right.withSizeKeepingCentre(58, 26));
        top.removeFromRight(gap); volR = top.removeFromRight(140); master.setBounds(volR.reduced(10, 4));
        top.removeFromRight(gap); metroR = top.removeFromRight(bw + 8); metro.setBounds(metroR.reduced(4, 3));
        top.removeFromRight(gap);
        lcd.setBounds(top.withSizeKeepingCentre(jmin(top.getWidth(), 600), top.getHeight()));
        insp.setVisible(inspOn); masterStrip.setVisible(mixOn); mixView.setVisible(mixOn); roll.setVisible(rollOn);
        if (inspOn) insp.setBounds(r.removeFromLeft(250));
        tuner.setVisible(tunerOn); if (tunerOn) tuner.setBounds(r.removeFromTop(TunerPanel::kH));
        if (mixOn || rollOn) { auto bot = r.removeFromBottom(jlimit(410, 500, r.getHeight() * 60 / 100));       
            if (mixOn && rollOn) roll.setBounds(bot.removeFromRight(bot.getWidth() / 2)); else if (rollOn) roll.setBounds(bot);
            if (mixOn) { masterStrip.setBounds(bot.removeFromRight(MixerPanel::stripW)); mixView.setBounds(bot); } }
        arrView.setBounds(r); arr.setSize(jmax(r.getWidth() - 14, Arrangement::head + (int) (p.length * arr.ppb) + 40), jmax(r.getHeight() - 14, Arrangement::ruler + (int) p.tracks.size() * Arrangement::laneH + 20));
        mix.setSize(MixerPanel::stripW * (int) p.tracks.size(), jmax(100, mixView.getHeight() - 14));
        start.setVisible(startOn); if (startOn) { start.setBounds(getLocalBounds().withTrimmedTop(menuBar ? 24 : 0)); start.toFront(false); }
    }
};
struct MiniDAWApp : JUCEApplication {
    struct Window : DocumentWindow {
        Window() : DocumentWindow("MiniDAW", Colours::black, allButtons) {
            setUsingNativeTitleBar(true); setContentOwned(new MainComponent(), true); setResizable(true, true); centreWithSize(getWidth(), getHeight()); setVisible(true); }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> w; String pendingOpen;
    static File projectFromCommandLine(const String& cl) {           
        auto test = [](const String& s) -> File { if (s.isEmpty()) return {}; const File f = File::getCurrentWorkingDirectory().getChildFile(s.unquoted()); return (MainComponent::isProjectFile(f) && f.existsAsFile()) ? f : File(); };
        if (auto f = test(cl.trim()); f != File()) return f;
        for (auto& t : StringArray::fromTokens(cl.trim(), " ", "\"'")) if (auto f = test(t); f != File()) return f;
        return {}; }
    void openInWindow(const File& f) { if (w != nullptr) if (auto* mc = dynamic_cast<MainComponent*>(w->getContentComponent())) { mc->openExternal(f); w->toFront(true); } }
    void anotherInstanceStarted(const String& commandLine) override {          
        const File f = projectFromCommandLine(commandLine); if (f == File()) return;
        if (w == nullptr) pendingOpen = f.getFullPathName(); else openInWindow(f); }
    const String getApplicationName() override { return "MiniDAW"; }
    const String getApplicationVersion() override { return "1.2.2"; }
    void runScanChild(const String& commandLine) {
#if JUCE_MAC
        Process::setDockIconVisible(false);
#endif
#if JUCE_MAC || JUCE_LINUX
        // Some plug-ins (e.g. AmpliTube) crash or hang while being destroyed at the end of a scan.
        // This process is throw-away: end only a crashing thread, and let a watchdog report what was found.
        { struct sigaction sa {}; sa.sa_handler = [](int) { pthread_exit(nullptr); }; sigemptyset(&sa.sa_mask);
          sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr); }
#endif
        auto a = StringArray::fromTokens(commandLine, true);   // [0]=--scan-plugin [1]=format [2]=plug-in id [3]=output file
        for (auto& t : a) t = t.unquoted();
        if (a.size() < 4) std::_Exit(1);
        struct Shared { OwnedArray<PluginDescription> found; std::atomic<bool> finished { false }, claimed { false }; File out; };
        auto* sh = new Shared();   // intentionally never freed: the process is about to exit
        sh->out = File(a[3]);
        auto report = [sh] { if (sh->claimed.exchange(true)) return;
            XmlElement root("FOUND"); for (auto* d : sh->found) root.addChildElement(d->createXml().release());
            const bool ok = root.writeTo(sh->out); std::_Exit(ok && ! sh->found.isEmpty() ? 0 : 2); };
        // watchdog: if the plug-in hangs after its description was read (e.g. in its destructor), report what we have
        std::thread([sh, report] {
            int lastCount = 0, stable = 0;
            for (int i = 0; i < 400; ++i) {   // 400 * 50 ms = 20 s
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (sh->finished.load()) return;
                const int n = sh->found.size();
                if (n > 0 && n == lastCount) { if (++stable >= 40) report(); } else { stable = 0; lastCount = n; } }
            std::_Exit(3); }).detach();
        AudioPluginFormatManager fm; fm.addDefaultFormats();
        for (auto* f : fm.getFormats()) if (f->getName() == a[1]) f->findAllTypesForFile(sh->found, a[2]);
        sh->finished = true;
        report(); }
    void initialise(const String& commandLine) override {
        if (commandLine.trim().startsWith("--scan-plugin")) { const auto cl = commandLine.trim(); MessageManager::callAsync([this, cl] { runScanChild(cl); }); return; }
        w = std::make_unique<Window>();
        File f = projectFromCommandLine(commandLine); if (pendingOpen.isNotEmpty()) f = File(pendingOpen); pendingOpen = {};
        if (f != File()) MessageManager::callAsync([this, f] { openInWindow(f); }); }
    void shutdown() override { w.reset(); }
    void systemRequestedQuit() override {                                                     
        if (w != nullptr) if (auto* mc = dynamic_cast<MainComponent*>(w->getContentComponent())) { mc->confirmDiscard([] { quit(); }); return; }
        quit(); }
};
START_JUCE_APPLICATION(MiniDAWApp)
