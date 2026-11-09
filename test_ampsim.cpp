#include "AmpSim.h"
#include <cstdio>
#include <cstdlib>
#include <random>
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } } while (0)
static std::vector<float> guitar(int n, double fs) {
    std::vector<float> v((size_t)n); std::mt19937 r(1); std::uniform_real_distribution<float> u(-1, 1);
    for (int i = 0; i < n; ++i) v[(size_t)i] = 0.4f * (float)std::sin(2 * ampsim::kPi * 196.0 * i / fs) + 0.02f * u(r);
    return v; }
static bool sane(const std::vector<float>& v) { for (float x : v) if (!std::isfinite(x) || std::fabs(x) > 8.f) return false; return true; }
static double rmsOf(const std::vector<float>& v, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += (double)v[i] * v[i]; return std::sqrt(s / (double)(b - a)); }

int main() {
    using namespace ampsim;
    for (const auto& m : pedalMetas()) for (int bs : { 1, 7, 64, 480, 4096, 9000 }) {
        StompPedal p(m.kind, 44100.0); auto in = guitar(bs * 3, 44100.0); std::vector<float> out(in.size());
        for (int o = 0; o < 3; ++o) p.process(in.data() + o * bs, out.data() + o * bs, bs);
        char b[128]; std::snprintf(b, sizeof b, "pedal %s block %d finite", m.key.c_str(), bs); CHECK(sane(out), b); }
    { StompPedal d(PedalKind::Delay, 44100.0); d.setParam("time", 800); d.setFs(96000.0);
      auto in = guitar(96000, 96000.0); std::vector<float> out(in.size());
      for (size_t o = 0; o + 512 <= in.size(); o += 512) d.process(in.data() + o, out.data() + o, 512);
      CHECK(sane(out), "delay after setFs(96k)");
      double e = 0; for (int i = 76800 + 100; i < 76800 + 2000; ++i) e += out[(size_t)i] * out[(size_t)i];
      CHECK(e > 0.01, "delay echo present at 96k"); }
    { Oversampler4 os; std::vector<float> in(4096, 0.5f), up(4 * 4096), out(4096);
      os.up(in.data(), up.data(), 4096); os.down(up.data(), out.data(), 4096);
      CHECK(std::fabs(out[4000] - 0.5f) < 1e-3f, "oversampler DC gain is 1");
      Oversampler4 o2; std::vector<float> s(8192), u2(4 * 8192), r(8192);
      for (int i = 0; i < 8192; ++i) s[(size_t)i] = 0.5f * (float)std::sin(2 * kPi * 4000.0 * i / 44100.0);
      o2.up(s.data(), u2.data(), 8192); o2.down(u2.data(), r.data(), 8192);
      CHECK(std::fabs(rmsOf(r, 4000, 8000) / rmsOf(s, 4000, 8000) - 1.0) < 0.02, "oversampler passband flat at 4 kHz"); }
    { Oversampler4 a, b; auto in = guitar(6000, 48000.0); std::vector<float> ua(4 * 6000), ub(4 * 6000), da(6000), db(6000);
      a.up(in.data(), ua.data(), 6000); a.down(ua.data(), da.data(), 6000);
      for (int o = 0; o < 6000; o += 100) { b.up(in.data() + o, ub.data() + 4 * o, 100); b.down(ub.data() + 4 * o, db.data() + o, 100); }
      double d = 0; for (int i = 0; i < 6000; ++i) d = std::max(d, (double)std::fabs(da[(size_t)i] - db[(size_t)i]));
      CHECK(d < 1e-5, "oversampler block independence"); }
    for (int voice = 0; voice < (int) voicings().size(); ++voice) {
      AmpSimProcessor a, b; a.prepare(48000); b.prepare(48000); a.params = presets()[3].p; a.params.voice = voice; a.params.reverb = 0.3f; b.params = a.params; a.recompute(); b.recompute();
      auto in = guitar(8192, 48000.0); std::vector<float> l1 = in, r1 = in, l2 = in, r2 = in;
      a.process(l1.data(), r1.data(), 4096); a.process(l1.data() + 4096, r1.data() + 4096, 4096);
      for (int o = 0; o < 8192; o += 64) b.process(l2.data() + o, r2.data() + o, 64);
      double d = 0; for (int i = 0; i < 8192; ++i) d = std::max(d, (double)std::fabs(l1[(size_t)i] - l2[(size_t)i]));
      char m[96]; std::snprintf(m, sizeof m, "amp output independent of block size (voice %d, diff %g)", voice, d);
      CHECK(d < 1e-4, m); CHECK(sane(l1) && sane(r1), "amp finite"); }
    for (const auto& pr : presets()) for (int ext = 0; ext < 3; ++ext) {
      AmpSimProcessor a; a.prepare(44100); a.params = pr.p;
      if (ext == 1) { a.params.drive = 1.f; a.params.master = 1.5f; a.params.sag = 1.f; a.params.bass_db = 12.f; a.params.mid_db = 12.f; a.params.treble_db = 12.f; a.params.reverb = 1.f; a.params.input_gain = 3.f; }
      if (ext == 2) { a.params.drive = 0.f; a.params.master = 0.f; a.params.tight = 1.f; a.params.mic = 1.f; }
      a.recompute();
      auto in = guitar(44100, 44100.0); std::vector<float> l = in, r = in;
      for (int o = 0; o + 333 <= 44100; o += 333) a.process(l.data() + o, r.data() + o, 333);
      float pk = 0; for (float x : l) pk = std::max(pk, std::fabs(x));
      char m[128]; std::snprintf(m, sizeof m, "preset %s extreme %d finite and <= 1.0 (peak %g)", pr.name, ext, pk);
      CHECK(sane(l) && sane(r) && pk <= 1.0001f, m); }
    { AmpSimProcessor a; a.prepare(44100); a.params = presets()[3].p; a.params.cab_lp_hz = 8000; a.recompute();
      std::vector<float> s(88200); for (size_t i = 0; i < s.size(); ++i) s[i] = 0.25f * (float)std::sin(2 * kPi * 1317.0 * (double)i / 44100.0);
      std::vector<float> l = s, r = s; for (size_t o = 0; o + 512 <= s.size(); o += 512) a.process(l.data() + o, r.data() + o, 512);
      double harm = 0, inh = 0; const size_t off = 30000;
      auto bin = [&](double f) { double re = 0, im = 0; for (int i = 0; i < 16384; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / 16384.0); const double v = l[off + (size_t) i] * w; re += v * std::cos(2 * kPi * f * (double) i / 44100.0); im += v * std::sin(2 * kPi * f * (double) i / 44100.0); } return re * re + im * im; };
      for (int h = 1; h <= 5; ++h) harm += bin(1317.0 * h);
      for (double f : { 1900.0, 2650.0, 3300.0, 4700.0, 5600.0, 6900.0 }) inh = std::max(inh, bin(f));
      CHECK(10 * std::log10(inh / harm + 1e-30) < -45.0, "aliasing below -45 dB"); }
    for (int bs : { 32, 1024 }) {
        auto run = [&](bool gate) { AmpSimProcessor a; a.prepare(44100); a.params.noise_gate_enabled = gate; a.params.noise_gate_thresh_db = -40;
            std::vector<float> l(44100), r; for (int i = 0; i < 44100; ++i) l[(size_t)i] = 0.002f * (float)std::sin(2 * kPi * 200.0 * i / 44100.0); r = l;
            for (int o = 0; o + bs <= 44100; o += bs) a.process(l.data() + o, r.data() + o, bs);
            float t = 0; for (int i = 38000; i < 42000; ++i) t = std::max(t, std::fabs(l[(size_t)i])); return t; };
        const float open = run(false), shut = run(true);
        char b[96]; std::snprintf(b, sizeof b, "gate closes (block %d): open=%g shut=%g", bs, open, shut); CHECK(open > 1e-3f && shut < open * 0.05f, b); }
    { AmpSimProcessor a; a.prepare(44100); a.params.noise_gate_enabled = true; a.params.noise_gate_thresh_db = -40;
      std::vector<float> l(44100 * 2), r; for (int i = 0; i < 44100; ++i) l[(size_t)i] = 0.001f * (float)std::sin(2 * kPi * 200.0 * i / 44100.0);
      for (int i = 44100; i < 88200; ++i) l[(size_t)i] = 0.3f * (float)std::sin(2 * kPi * 200.0 * i / 44100.0);
      r = l;
      for (int o = 0; o + 256 <= 88200; o += 256) a.process(l.data() + o, r.data() + o, 256);
      CHECK(rmsOf(l, 60000, 80000) > 0.02, "gate opens again on loud signal"); }
    for (int wet = 0; wet < 2; ++wet) {
      AmpSimProcessor a; a.prepare(44100); a.params = presets()[0].p; a.params.reverb = wet ? 0.8f : 0.f; a.recompute();
      std::vector<float> l(44100 * 2, 0.f), r; for (int i = 0; i < 4000; ++i) l[(size_t)i] = 0.3f * (float)std::sin(2 * kPi * 330.0 * i / 44100.0); r = l;
      for (int o = 0; o + 512 <= 88200; o += 512) a.process(l.data() + o, r.data() + o, 512);
      const double tail = rmsOf(l, 12000, 16000);
      CHECK(wet ? tail > 1e-4 : tail < 1e-6, wet ? "reverb tail present" : "no tail with reverb off");
      if (wet) { double d = 0; for (int i = 12000; i < 16000; ++i) d = std::max(d, (double)std::fabs(l[(size_t)i] - r[(size_t)i])); CHECK(d > 1e-6, "reverb is stereo"); } }
    { AmpSimProcessor a; a.prepare(44100); a.params = presets()[9].p; a.prepare(96000); a.params.reverb = 0.4f;
      auto in = guitar(96000, 96000.0); std::vector<float> l = in, r = in; for (size_t o = 0; o + 480 <= in.size(); o += 480) a.process(l.data() + o, r.data() + o, 480);
      CHECK(sane(l) && sane(r), "amp finite after prepare(96k)"); }
    { AmpSimProcessor a; a.params.drive = 0.8f; a.params.voice = 3; a.params.reverb = 0.25f; a.recompute(); a.pedalboard.add(PedalKind::TS9)->setParam("drive", 0.9);
      AmpSimProcessor c(a); CHECK(c.pedalboard.pedals.size() == 1 && c.params.drive == 0.8f && c.params.voice == 3 && c.params.reverb == 0.25f, "copy keeps state");
      CHECK(c.pedalboard.pedals[0].get() != a.pedalboard.pedals[0].get(), "copy is deep");
      auto in = guitar(88200, 44100.0); std::vector<float> l1 = in, r1 = in, l2 = in, r2 = in;
      a.process(l1.data(), r1.data(), 88200); c.process(l2.data(), r2.data(), 88200);
      double d = 0; for (int i = 78200; i < 88200; ++i) d = std::max(d, (double)std::fabs(l1[(size_t)i] - l2[(size_t)i]));
      CHECK(d < 1e-4, "copy sounds identical"); }
    { double e[4] = {}; for (int v = 0; v < 4; ++v) { AmpSimProcessor a; a.prepare(44100); a.params = presets()[5].p; a.params.voice = v; a.recompute();
        auto in = guitar(22050, 44100.0); std::vector<float> l = in, r = in; a.process(l.data(), r.data(), 22050); e[v] = rmsOf(l, 4000, 22000); }
      CHECK(std::fabs(e[0] - e[3]) > 1e-3 || std::fabs(e[1] - e[2]) > 1e-3, "voicings are distinct"); }
    for (const auto& m : pedalMetas()) {
        bool known = false; for (const char* c : pedalCategories()) if (std::string(c) == categoryOf(m.kind)) known = true;
        char b[96]; std::snprintf(b, sizeof b, "pedal %s has a category", m.key.c_str()); CHECK(known, b);
        int dup = 0; for (const auto& o : pedalMetas()) if (o.key == m.key) ++dup;
        std::snprintf(b, sizeof b, "pedal key %s unique", m.key.c_str()); CHECK(dup == 1, b);
        StompPedal p(m.kind, 44100.0); auto in = guitar(44100, 44100.0); std::vector<float> out(in.size());
        for (size_t o = 0; o + 512 <= in.size(); o += 512) p.process(in.data() + o, out.data() + o, 512);
        std::snprintf(b, sizeof b, "pedal %s produces sound (rms %g)", m.key.c_str(), rmsOf(out, 20000, 40000)); CHECK(rmsOf(out, 20000, 40000) > 1e-3, b); }
    for (PedalKind k : { PedalKind::Wah, PedalKind::EnvFilter, PedalKind::Univibe, PedalKind::HarmTrem, PedalKind::Tape, PedalKind::NoiseGate, PedalKind::Reverb, PedalKind::Octaver, PedalKind::BD2 }) {
        StompPedal a(k, 44100.0), b(k, 44100.0); auto in = guitar(8192, 44100.0); std::vector<float> oa(in.size()), ob(in.size());
        a.process(in.data(), oa.data(), 8192); for (int o = 0; o < 8192; o += 128) b.process(in.data() + o, ob.data() + o, 128);
        double d = 0; for (size_t i = 0; i < in.size(); ++i) d = std::max(d, (double)std::fabs(oa[i] - ob[i]));
        char m[96]; std::snprintf(m, sizeof m, "pedal %s block independent (diff %g)", metaOf(k)->key.c_str(), d); CHECK(d < 2e-3, m); }
    { StompPedal o(PedalKind::Octaver, 44100.0); o.setParam("oct1", 1.0); o.setParam("direct", 0.0); o.setParam("oct2", 0.0);
      std::vector<float> in(44100), out(44100); for (int i = 0; i < 44100; ++i) in[(size_t)i] = 0.4f * (float) std::sin(2 * kPi * 220.0 * i / 44100.0);
      for (size_t k = 0; k + 512 <= in.size(); k += 512) o.process(in.data() + k, out.data() + k, 512);
      auto bin = [&](double f) { double re = 0, im = 0; for (int i = 10000; i < 40000; ++i) { re += out[(size_t)i] * std::cos(2 * kPi * f * i / 44100.0); im += out[(size_t)i] * std::sin(2 * kPi * f * i / 44100.0); } return re * re + im * im; };
      CHECK(bin(110.0) > 4.0 * bin(220.0), "octaver puts its energy an octave below"); }
    { StompPedal g(PedalKind::NoiseGate, 44100.0); std::vector<float> in(44100), out(44100);
      for (int i = 0; i < 22050; ++i) in[(size_t)i] = 0.0003f * (float) std::sin(2 * kPi * 150.0 * i / 44100.0);
      for (int i = 22050; i < 44100; ++i) in[(size_t)i] = 0.3f * (float) std::sin(2 * kPi * 150.0 * i / 44100.0);
      for (size_t k = 0; k + 256 <= in.size(); k += 256) g.process(in.data() + k, out.data() + k, 256);
      CHECK(rmsOf(out, 15000, 22000) < 0.0002 && rmsOf(out, 30000, 44000) > 0.15, "ns2 closes on hiss, passes notes"); }
    CHECK(StompPedal(PedalKind::Delay, 44100.0).post && StompPedal(PedalKind::Tape, 44100.0).post && StompPedal(PedalKind::Reverb, 44100.0).post && !StompPedal(PedalKind::TS9, 44100.0).post, "default pedal positions");
    for (int post = 0; post < 2; ++post) {
      AmpSimProcessor a; a.prepare(44100); a.params = presets()[3].p; a.recompute();
      auto* d = a.pedalboard.add(PedalKind::Delay); d->setParam("time", 300); d->setParam("level", 0.8); d->post = post != 0;
      std::vector<float> l(44100, 0.f), r; for (int i = 0; i < 2000; ++i) l[(size_t)i] = 0.3f * (float) std::sin(2 * kPi * 330.0 * i / 44100.0); r = l;
      for (size_t o = 0; o + 512 <= l.size(); o += 512) a.process(l.data() + o, r.data() + o, 512);
      CHECK(sane(l) && rmsOf(l, 14000, 17000) > 1e-3, post ? "post delay echoes" : "pre delay echoes"); }
    { AmpSimProcessor a; a.pedalboard.add(PedalKind::Reverb); a.pedalboard.add(PedalKind::TS9); AmpSimProcessor c(a);
      CHECK(c.pedalboard.pedals[0]->post && !c.pedalboard.pedals[1]->post, "copy keeps pedal positions"); }
    for (const auto& bp : boardPresets()) {
      AmpSimProcessor a; a.prepare(44100); a.params = presets()[1].p; a.recompute();
      const int n = loadBoardPreset(a.pedalboard, bp);
      char m[96]; std::snprintf(m, sizeof m, "rig %s loads all %d pedals", bp.name, (int) bp.slots.size()); CHECK(n == (int) bp.slots.size(), m);
      auto in = guitar(44100, 44100.0); std::vector<float> l = in, r = in;
      for (size_t o = 0; o + 333 <= in.size(); o += 333) a.process(l.data() + o, r.data() + o, 333);
      float pk = 0; for (float x : l) pk = std::max(pk, std::fabs(x));
      std::snprintf(m, sizeof m, "rig %s finite and bounded (peak %g)", bp.name, pk); CHECK(sane(l) && sane(r) && pk <= 1.0001f && pk > 1e-3f, m); }
    { PedalBoard pb; for (int i = 0; i < 15; ++i) pb.add(PedalKind::OD1); CHECK((int) pb.pedals.size() == PedalBoard::kMaxPedals && PedalBoard::kMaxPedals == 12, "max 12 pedals"); }
    { PedalBoard pb; pb.add(PedalKind::DS1); pb.add(PedalKind::TS9); pb.move(0, 1); CHECK(pb.pedals[0]->kind == PedalKind::TS9, "move"); pb.removeAt(0); CHECK(pb.pedals.size() == 1, "remove"); }
    CHECK(presets().size() == 28 && cabSims().size() == 11 && voicings().size() == 4, "preset/cab/voicing counts");

    // ─── custom .mdpd pedal tests ──────────────────────────────────────────
    {
        std::string err;
        auto def = mdp::parseSource(R"(
            pedal "Test Gain"
            knob Gain = 5
            on sample(x) { return x * Gain * 0.2; }
        )", err);
        CHECK(def != nullptr && err.empty(), "parse valid .mdpd");
        auto bad = mdp::parseSource("pedal \"x\" on sample(x) { }", err);
        CHECK(bad == nullptr && ! err.empty(), "reject pedal without return");
        const auto key = registerCustomPedal(def);
        CHECK(key == "test_gain", "custom pedal key");
        CHECK(findMeta(key) != nullptr, "findMeta sees the custom pedal");
        StompPedal sp(def, 44100.0);
        CHECK(sp.kind == PedalKind::Custom, "custom pedal kind");
        CHECK(sp.meta() != nullptr && sp.meta()->display == "Test Gain", "custom meta");
        auto in = guitar(44100, 44100.0); std::vector<float> out(in.size());
        for (size_t o = 0; o + 512 <= in.size(); o += 512) sp.process(in.data() + o, out.data() + o, 512);
        CHECK(sane(out), "custom pedal finite");
        CHECK(rmsOf(out, 20000, 40000) > 1e-3, "custom pedal audible");
        auto cl = sp.clone();
        CHECK(cl->kind == PedalKind::Custom && cl->customDef != nullptr, "custom clone");
        std::vector<float> out2(in.size());
        for (size_t o = 0; o + 512 <= in.size(); o += 512) cl->process(in.data() + o, out2.data() + o, 512);
        double d = 0; for (size_t i = 0; i < in.size(); ++i) d = std::max(d, (double) std::fabs(out[i] - out2[i]));
        CHECK(d < 1e-6, "custom copy sounds identical");
        auto def2 = mdp::parseSource(R"(
            pedal "Test LP"
            knob Tone = 5
            state lp = 0
            on sample(x) {
                lp = lp + (x - lp) * (0.01 + Tone * 0.01);
                return lp;
            }
        )", err);
        CHECK(def2 != nullptr, "parse state-variable pedal");
        if (def2) {
            StompPedal sp2(def2, 44100.0); std::vector<float> o3(in.size());
            for (size_t o = 0; o + 512 <= in.size(); o += 512) sp2.process(in.data() + o, o3.data() + o, 512);
            CHECK(sane(o3), "state pedal finite");
        }
        auto def3 = mdp::parseSource(R"(
            pedal "Test LFO"
            knob Rate = 5
            state phase = 0
            on sample(x) {
                phase = phase + Rate * 10.0 * dt;
                return sin(phase) * x;
            }
        )", err);
        CHECK(def3 != nullptr, "parse dt pedal");
        auto bad2 = mdp::parseSource("pedal \"x\" knob A = 5 on sample(x) { return x * B; }", err);
        CHECK(bad2 == nullptr && err.find("unknown identifier") != std::string::npos, "reject unknown identifier");
        auto bad3 = mdp::parseSource("pedal \"x\" knob x = 5 on sample(x) { return x; }", err);
        CHECK(bad3 == nullptr && ! err.empty(), "reject reserved knob name");
        PedalBoard pb;
        for (int i = 0; i < 20; ++i) pb.add(def);
        CHECK((int) pb.pedals.size() == PedalBoard::kMaxPedals, "custom pedals respect max board size");
    }

    std::printf(fails ? "%d FAILED\n" : "all AmpSim tests passed\n", fails); return fails ? 1 : 0;
}
