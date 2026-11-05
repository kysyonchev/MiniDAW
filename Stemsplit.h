#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Stemsplit.h"
using namespace juce;
namespace stemui {
constexpr int kNumStems = 6;      
static const char* const kKeys[kNumStems] = { "guitar", "piano", "bass", "vocals", "drums", "other" };
inline int stemIndex(const String& key) { for (int i = 0; i < kNumStems; ++i) if (key == kKeys[i]) return i; return -1; }
constexpr int kNumModels = 6;     
struct ModelInfo { const char* key; const char* sub; int files; const char* help; };
static const ModelInfo kModelInfo[kNumModels] = {
    { "htdemucs",     "4 stems - balanced speed and quality (recommended)", 1, "The default Hybrid Transformer Demucs (v4). Splits into Vocals, Drums, Bass and Other. The best balance of quality and speed - start here." },
    { "htdemucs_ft",  "4 stems - best quality, about 4x slower",            4, "A fine-tuned version of htdemucs with one specialised network per stem (4 networks, so 4 files to download). Slightly cleaner results, roughly 4x slower." },
    { "htdemucs_6s",  "6 stems - adds Guitar and Piano (experimental)",     1, "Experimental 6-stem version: adds Guitar and Piano. Guitar usually works well; piano is weaker and can sound smeared. Use it when you need those two stems." },
    { "mdx_extra",    "4 stems - alternative model, extra training data",   4, "A Hybrid Demucs from the MDX music separation challenge (a bag of 4 networks). A good second opinion if htdemucs leaves artefacts on a particular song." },
    { "hdemucs_mmi",  "4 stems - older Hybrid Demucs v3, different sound",  1, "Hybrid Demucs v3 retrained with extra data. An older design than htdemucs, so it has a different character and is sometimes better on certain mixes." },
    { "mdx_extra_q",  "4 stems - compressed mdx_extra, smaller download",   4, "A compressed (quantised) mdx_extra: smaller download and less memory for a small loss of quality." } };
}
struct StemSettings {
    int engine = 0;                                                          
    int quality = 1; float sharpness = 2.f, sustainMs = 190.f, drumWidthHz = 700.f, bassHz = 180.f, guitarTopHz = 500.f, pianoTopHz = 1500.f, vocalTopHz = 4000.f;   
    int qMask = 63;                                                          
    int model = 0, mode = 0, device = 0, shifts = 1; float overlap = 0.25f; String exe;                                                                
    int aMask = 63;                                                          
    int installed = 0;                                                       
    bool instrumental = false;                                               
    bool muteOriginal = true, saveFiles = false;                                                                                                         
    void save(XmlElement& x) const {
        x.setAttribute("engine", engine); x.setAttribute("quality", quality); x.setAttribute("sharp", (double) sharpness); x.setAttribute("sustain", (double) sustainMs);
        x.setAttribute("drumW", (double) drumWidthHz); x.setAttribute("bassHz", (double) bassHz); x.setAttribute("gTop", (double) guitarTopHz); x.setAttribute("pTop", (double) pianoTopHz); x.setAttribute("vTop", (double) vocalTopHz);
        x.setAttribute("qMask", qMask); x.setAttribute("aMask", aMask); x.setAttribute("installed", installed); x.setAttribute("instr", instrumental ? 1 : 0);
        x.setAttribute("model", model); x.setAttribute("mode", mode); x.setAttribute("device", device); x.setAttribute("shifts", shifts); x.setAttribute("overlap", (double) overlap); x.setAttribute("exe", exe);
        x.setAttribute("mute", muteOriginal ? 1 : 0); x.setAttribute("saveFiles", saveFiles ? 1 : 0); }
    void load(const XmlElement& x) {
        engine = jlimit(0, 1, x.getIntAttribute("engine", 0)); quality = jlimit(0, 2, x.getIntAttribute("quality", 1)); sharpness = (float) jlimit(1.0, 4.0, x.getDoubleAttribute("sharp", 2.0));
        sustainMs = (float) jlimit(60.0, 500.0, x.getDoubleAttribute("sustain", 190.0)); drumWidthHz = (float) jlimit(200.0, 2000.0, x.getDoubleAttribute("drumW", 700.0)); bassHz = (float) jlimit(60.0, 400.0, x.getDoubleAttribute("bassHz", 180.0));
        guitarTopHz = (float) jlimit(250.0, 1000.0, x.getDoubleAttribute("gTop", 500.0)); pianoTopHz = (float) jlimit(600.0, 3000.0, x.getDoubleAttribute("pTop", 1500.0)); vocalTopHz = (float) jlimit(2000.0, 8000.0, x.getDoubleAttribute("vTop", 4000.0));
        qMask = x.getIntAttribute("qMask", 63) & 63; if (qMask == 0) qMask = 63; aMask = x.getIntAttribute("aMask", 63) & 63; if (aMask == 0) aMask = 63;
        installed = x.getIntAttribute("installed", 0) & ((1 << stemui::kNumModels) - 1); instrumental = x.getIntAttribute("instr", 0) != 0;
        model = jlimit(0, stemui::kNumModels - 1, x.getIntAttribute("model", 0)); mode = jlimit(0, 3, x.getIntAttribute("mode", 0)); device = jlimit(0, 3, x.getIntAttribute("device", 0)); shifts = jlimit(0, 5, x.getIntAttribute("shifts", 1));
        overlap = (float) jlimit(0.1, 0.5, x.getDoubleAttribute("overlap", 0.25)); exe = x.getStringAttribute("exe"); muteOriginal = x.getIntAttribute("mute", 1) != 0; saveFiles = x.getIntAttribute("saveFiles", 0) != 0; }
};
struct StemOut { String name; Colour colour; std::shared_ptr<std::vector<float>> wav; double sr = 44100; File file; };    
namespace stemui {
struct StemInfo { const char* key; const char* name; juce::uint32 argb; };
inline const StemInfo* infoFor(const String& key) {
    static const StemInfo t[] = { { "vocals", "Vocals", 0xffe85d8f }, { "drums", "Drums", 0xfff2a03d }, { "bass", "Bass", 0xff4f8cf0 }, { "guitar", "Guitar", 0xffe9cb45 }, { "piano", "Piano", 0xff9b7bf0 },
                                  { "other", "Other", 0xff45c78c }, { "melody", "Melody", 0xff45c78c }, { "no_vocals", "Instrumental", 0xff8a8d96 }, { "no_drums", "No Drums", 0xff8a8d96 }, { "no_bass", "No Bass", 0xff8a8d96 }, { "rest", "Rest", 0xff8a8d96 } };
    for (auto& i : t) if (key == i.key) return &i;
    return nullptr; }
inline float textW(const Font& f, const String& t) { GlyphArrangement ga; ga.addLineOfText(f, t, 0.f, 0.f); return ga.getBoundingBox(0, -1, true).getWidth(); }
inline Colour colourFor(const String& key) { auto* i = infoFor(key); return Colour(i != nullptr ? i->argb : 0xff8a8d96); }
inline String nameFor(const String& key) { auto* i = infoFor(key); return i != nullptr ? String(i->name) : key.substring(0, 1).toUpperCase() + key.substring(1); }
inline int orderOf(const String& key) { static const char* o[] = { "guitar", "piano", "bass", "vocals", "drums", "other", "melody", "no_vocals", "no_drums", "no_bass", "rest" }; for (int i = 0; i < 11; ++i) if (key == o[i]) return i; return 50; }
static const char* const kTwoStems[4] = { "", "vocals", "drums", "bass" };
inline bool writeWav(const File& f, const float* d, int n, double sr) {                      
    f.deleteFile(); std::unique_ptr<FileOutputStream> fo(f.createOutputStream()); if (! fo) return false;
    WavAudioFormat wav; std::unique_ptr<AudioFormatWriter> w(wav.createWriterFor(fo.get(), sr, 1u, 24, {}, 0)); if (! w) return false;
    fo.release(); const float* ch[] = { d }; return w->writeFromFloatArrays(ch, 1, n); }
inline std::shared_ptr<std::vector<float>> readMono(const File& f, double& sr) {                
    AudioFormatManager afm; afm.registerBasicFormats(); std::unique_ptr<AudioFormatReader> rd(afm.createReaderFor(f)); if (! rd) return nullptr;
    const int n = (int) jmin((int64) rd->lengthInSamples, (int64) (rd->sampleRate * 3600.0)); AudioBuffer<float> b(jmax(1, (int) rd->numChannels), jmax(1, n)); if (! rd->read(&b, 0, n, 0, true, true)) return nullptr;
    auto v = std::make_shared<std::vector<float>>((size_t) n); for (int i = 0; i < n; ++i) { float s = 0; for (int ch = 0; ch < b.getNumChannels(); ++ch) s += b.getSample(ch, i); (*v)[(size_t) i] = s / (float) b.getNumChannels(); }
    sr = rd->sampleRate; return v; }
inline File envDir() {
#if JUCE_MAC
    return File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("Application Support/MiniDAW/demucs-env");
#else
    return File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("MiniDAW/demucs-env");
#endif
}
inline File venvBin(const String& name) {
#if JUCE_WINDOWS
    return envDir().getChildFile("Scripts").getChildFile(name + ".exe");
#else
    return envDir().getChildFile("bin").getChildFile(name);
#endif
}
inline File venvPython() { return venvBin("python"); }
inline File venvDemucs() { return venvBin("demucs"); }
inline File findDemucs(const String& userChoice) {
    if (userChoice.isNotEmpty() && File(userChoice).existsAsFile()) return File(userChoice);
    { const File v = venvDemucs(); if (v.existsAsFile()) return v; }                                            
    const File home = File::getSpecialLocation(File::userHomeDirectory); Array<File> c;
    c.add(home.getChildFile("minidaw-demucs/bin/demucs")); c.add(home.getChildFile("minidaw-demucs/Scripts/demucs.exe")); c.add(home.getChildFile(".local/bin/demucs")); c.add(File("/opt/homebrew/bin/demucs")); c.add(File("/usr/local/bin/demucs")); c.add(home.getChildFile("miniconda3/bin/demucs"));
    c.add(home.getChildFile("miniforge3/bin/demucs")); c.add(home.getChildFile("anaconda3/bin/demucs")); c.add(home.getChildFile(".pyenv/shims/demucs")); c.add(File("/usr/bin/demucs"));
    for (auto& d : home.getChildFile("Library/Python").findChildFiles(File::findDirectories, false)) c.add(d.getChildFile("bin/demucs"));
    for (auto& f : c) if (f.existsAsFile()) return f;
#if JUCE_WINDOWS
    ChildProcess cp; if (cp.start("where demucs")) { auto out = StringArray::fromLines(cp.readAllProcessOutput().trim()); if (out.size() > 0 && File(out[0].trim()).existsAsFile()) return File(out[0].trim()); }
#else
    ChildProcess cp; if (cp.start(StringArray { File("/bin/zsh").existsAsFile() ? "/bin/zsh" : "/bin/sh", "-lc", "command -v demucs" })) {
        auto lines = StringArray::fromLines(cp.readAllProcessOutput().trim()); for (int i = lines.size(); --i >= 0;) { const File f(lines[i].trim()); if (File::isAbsolutePath(f.getFullPathName()) && f.existsAsFile()) return f; } }
#endif
    return {}; }
inline File findPython(const File& demucs) {
    { FileInputStream in(demucs);                                                                                    
      if (in.openedOk()) { char b[512] = {}; const int n = in.read(b, 511); const auto lines = StringArray::fromLines(String::fromUTF8(b, jmax(0, n)));
          if (lines.size() > 0 && lines[0].startsWith("#!")) {
              const auto tok = StringArray::fromTokens(lines[0].substring(2).trim(), " ", "\"");
              if (tok.size() > 0 && ! tok[0].endsWith("/env") && ! tok[0].endsWith("/sh") && ! tok[0].endsWith("/bash") && File(tok[0]).existsAsFile()) return File(tok[0]);
              if (lines.size() > 1 && lines[1].contains("exec")) {                                                  
                  const int q1 = lines[1].indexOfChar('"'); const int q2 = q1 >= 0 ? lines[1].indexOfChar(q1 + 1, '"') : -1;
                  if (q2 > q1) { const File f(lines[1].substring(q1 + 1, q2)); if (f.existsAsFile()) return f; } } } } }
    for (auto* nm : { "python3", "python", "python3.exe", "python.exe" }) { const File f = demucs.getSiblingFile(nm); if (f.existsAsFile()) return f; }   
#if JUCE_WINDOWS
    { const File f = demucs.getParentDirectory().getParentDirectory().getChildFile("python.exe"); if (f.existsAsFile()) return f; }
    ChildProcess cp; if (cp.start("where python")) { auto out = StringArray::fromLines(cp.readAllProcessOutput().trim()); if (out.size() > 0 && File(out[0].trim()).existsAsFile()) return File(out[0].trim()); }
#else
    ChildProcess cp; if (cp.start(StringArray { File("/bin/zsh").existsAsFile() ? "/bin/zsh" : "/bin/sh", "-lc", "command -v python3" })) {   
        auto lines = StringArray::fromLines(cp.readAllProcessOutput().trim()); for (int i = lines.size(); --i >= 0;) { const File f(lines[i].trim()); if (File::isAbsolutePath(f.getFullPathName()) && f.existsAsFile()) return f; } }
#endif
    return {}; }
struct DlShared {                                                                           
    std::atomic<float> progress { 0.f }; std::atomic<bool> cancel { false }; CriticalSection lock; ChildProcess* proc = nullptr; String status;
    void setStatus(const String& s) { const ScopedLock l(lock); status = s; }
    String getStatus() { const ScopedLock l(lock); return status; }
    void killNow() { cancel = true; const ScopedLock l(lock); if (proc != nullptr) proc->kill(); } };
inline int runStreamed(const std::shared_ptr<DlShared>& sh, const StringArray& args, String& tail, const std::function<void(const String&)>& onChunk) {
    ChildProcess cp;
    { const ScopedLock l(sh->lock); if (sh->cancel.load()) return -2; if (! cp.start(args)) { tail = "Could not start " + args[0]; return -1; } sh->proc = &cp; }
    char buf[2048];
    for (;;) {
        const int n = cp.readProcessOutput(buf, (int) sizeof(buf) - 1); if (n <= 0) break;
        const String chunk = String::fromUTF8(buf, n); tail = (tail + chunk).getLastCharacters(1600); if (onChunk) onChunk(chunk);
        if (sh->cancel.load()) { cp.kill(); break; } }
    cp.waitForProcessToFinish(5000); { const ScopedLock l(sh->lock); sh->proc = nullptr; }
    return sh->cancel.load() ? -2 : (int) cp.getExitCode(); }
inline bool pythonUsable(const File& py) {
    if (! py.existsAsFile()) return false;
    ChildProcess cp; if (! cp.start(StringArray { py.getFullPathName(), "-c", "import sys, venv; v = sys.version_info; print('PYOK' if (3, 9) <= (v[0], v[1]) <= (3, 13) else 'PYBAD')" })) return false;
    return cp.readAllProcessOutput().contains("PYOK"); }
inline File findSystemPython() {
    const File home = File::getSpecialLocation(File::userHomeDirectory); Array<File> c;
#if JUCE_WINDOWS
    for (auto& d : home.getChildFile("AppData/Local/Programs/Python").findChildFiles(File::findDirectories, false, "Python3*")) c.add(d.getChildFile("python.exe"));
    for (auto* v : { "312", "311", "310", "313", "39" }) { c.add(File("C:/Program Files/Python" + String(v) + "/python.exe")); c.add(File("C:/Python" + String(v) + "/python.exe")); }
#else
    for (auto* nm : { "python3.12", "python3.11", "python3.10", "python3.13", "python3", "python3.9" }) for (auto* dir : { "/opt/homebrew/bin/", "/usr/local/bin/" }) c.add(File(String(dir) + nm));
    for (auto* v : { "3.12", "3.11", "3.10", "3.13", "3.9" }) c.add(File("/Library/Frameworks/Python.framework/Versions/" + String(v) + "/bin/python3"));
    c.add(File("/usr/bin/python3"));
#endif
    for (auto& f : c) if (pythonUsable(f)) return f;
#if JUCE_WINDOWS
    ChildProcess cp; if (cp.start("where python")) { for (auto& l : StringArray::fromLines(cp.readAllProcessOutput().trim())) { const File f(l.trim()); if (! f.getFullPathName().containsIgnoreCase("WindowsApps") && pythonUsable(f)) return f; } }
#else
    ChildProcess cp; if (cp.start(StringArray { File("/bin/zsh").existsAsFile() ? "/bin/zsh" : "/bin/sh", "-lc", "for p in python3.12 python3.11 python3.10 python3; do command -v $p; done" })) {
        for (auto& l : StringArray::fromLines(cp.readAllProcessOutput().trim())) { const File f(l.trim()); if (File::isAbsolutePath(f.getFullPathName()) && pythonUsable(f)) return f; } }
#endif
    return {}; }
inline bool installDemucs(const std::shared_ptr<DlShared>& sh, String& err, float& baseOut) {
    sh->setStatus("Looking for Python..."); sh->progress = 0.02f;
    const File py = findSystemPython();
    if (! py.existsAsFile()) { err = "MiniDAW needs Python 3.9 - 3.13 to set up Demucs, and could not find it.\n\nInstall Python from python.org (on Windows tick \"Add python.exe to PATH\"), then press Download again."; return false; }
    String tail; const File env = envDir();
    if (env.exists() && ! venvPython().existsAsFile()) env.deleteRecursively();                  
    if (! venvPython().existsAsFile()) {
        sh->setStatus("Creating a private Python environment..."); sh->progress = 0.05f; env.getParentDirectory().createDirectory();
        const int rc = runStreamed(sh, StringArray { py.getFullPathName(), "-m", "venv", env.getFullPathName() }, tail, {});
        if (rc == -2) return false;
        if (rc != 0 || ! venvPython().existsAsFile()) { err = "Could not create the Python environment.\n\n" + tail.trim().getLastCharacters(500) + "\n\nOn Debian / Ubuntu install the venv package first:  sudo apt install python3-venv"; return false; } }
    sh->setStatus("Preparing the installer..."); sh->progress = 0.08f;
    { String t2; const int rc = runStreamed(sh, StringArray { venvPython().getFullPathName(), "-m", "pip", "install", "--disable-pip-version-check", "--upgrade", "pip" }, t2, {}); if (rc == -2) return false; }
    sh->setStatus("Installing Demucs (one-time, a few minutes)..."); float p = 0.10f; sh->progress = p;
    auto onPip = [&](const String& chunk) {
        for (auto& line : StringArray::fromLines(chunk)) { const String t = line.trim();
            if (t.startsWith("Collecting ")) { const String pkg = t.substring(11).trim().initialSectionNotContaining(" <>=!~;("); if (pkg.isNotEmpty()) sh->setStatus("Installing Demucs - fetching " + pkg + "..."); p += (0.60f - p) * 0.10f; }
            else if (t.startsWith("Downloading ")) p += (0.60f - p) * 0.04f;
            else if (t.startsWith("Installing collected")) { sh->setStatus("Installing Demucs - almost done..."); p = 0.60f; }
            sh->progress = p; } };
    const int rc = runStreamed(sh, StringArray { venvPython().getFullPathName(), "-m", "pip", "install", "--disable-pip-version-check", "torch<2.9", "torchaudio<2.9", "soundfile", "certifi", "demucs" }, tail, onPip);
    if (rc == -2) return false;
    if (rc != 0 || ! venvDemucs().existsAsFile()) { err = "Installing Demucs failed.\n\n" + tail.trim().getLastCharacters(600); return false; }
    baseOut = 0.62f; return true; }
inline bool downloadModel(const std::shared_ptr<DlShared>& sh, const String& userChoice, int model, String& err, File& exeOut) {
    float base = 0.02f; File exe = findDemucs(userChoice);
    if (! exe.existsAsFile()) { if (! installDemucs(sh, err, base)) return false; exe = venvDemucs(); }
    exeOut = exe; if (sh->cancel.load()) return false;
    const File py = exe == venvDemucs() ? venvPython() : findPython(exe); if (! py.existsAsFile()) { err = "Could not find the Python that Demucs runs on."; return false; }
    const String key = kModelInfo[model].key; const int files = jmax(1, kModelInfo[model].files);
    sh->setStatus("Contacting the download server..."); sh->progress = base;
    const String code = "import ssl\ntry:\n    import certifi\n    ssl._create_default_https_context = lambda: ssl.create_default_context(cafile=certifi.where())\nexcept Exception:\n    pass\n"
                        "from demucs.pretrained import get_model\nget_model('" + key + "')\nprint('MINIDAW_MODEL_OK')\n";
    String tail; int file = 0, lastPct = -1;
    const int rc = runStreamed(sh, StringArray { py.getFullPathName(), "-u", "-c", code }, tail, [&](const String& chunk) {
        const int pc = chunk.lastIndexOfChar('%'); if (pc <= 0) return;
        int st = pc; while (st > 0 && CharacterFunctions::isDigit(chunk[st - 1])) --st; if (st >= pc) return;
        const int pct = jlimit(0, 100, chunk.substring(st, pc).getIntValue()); if (pct + 20 < lastPct) file = jmin(files - 1, file + 1); lastPct = pct;      
        sh->progress = base + (0.99f - base) * jmin(1.f, ((float) file + (float) pct / 100.f) / (float) files);
        sh->setStatus(files > 1 ? "Downloading " + key + " (file " + String(file + 1) + " of " + String(files) + ")..." : "Downloading " + key + "..."); });
    if (rc == -2) return false;
    if (rc != 0 || ! tail.contains("MINIDAW_MODEL_OK")) { err = tail.trim().getLastCharacters(700); if (err.isEmpty()) err = "The download did not finish."; return false; }
    sh->progress = 1.f; return true; }
}   
struct StemSegmented : Component {                                                          
    struct Item { String title, sub; }; std::vector<Item> items; int sel = 0, hot = -1; std::function<void(int)> onChange;
    int indexAt(Point<int> p) const { return jlimit(0, jmax(0, (int) items.size() - 1), (int) (p.x * (int) items.size() / jmax(1, getWidth()))); }
    void paint(Graphics& g) override {
        const Colour acc = getLookAndFeel().findColour(Slider::trackColourId); const auto r = getLocalBounds().toFloat(); const float a = isEnabled() ? 1.f : 0.5f;
        g.setColour(Colour(0xff1c1d21)); g.fillRoundedRectangle(r, 13.f); g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(r.reduced(0.5f), 13.f, 1.f);
        const float w = r.getWidth() / (float) jmax(1, (int) items.size());
        for (int i = 0; i < (int) items.size(); ++i) {
            const auto b = Rectangle<float>(r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced(4.f);
            if (i == sel) { g.setGradientFill(ColourGradient(acc.withAlpha(0.32f * a), b.getX(), b.getY(), acc.withAlpha(0.14f * a), b.getX(), b.getBottom(), false)); g.fillRoundedRectangle(b, 10.f); g.setColour(acc.withAlpha(0.85f * a)); g.drawRoundedRectangle(b.reduced(0.5f), 10.f, 1.2f); }
            else if (i == hot && isEnabled()) { g.setColour(Colour(0x10ffffff)); g.fillRoundedRectangle(b, 10.f); }
            g.setColour((i == sel ? Colours::white : Colour(0xffc0c1c8)).withMultipliedAlpha(a)); g.setFont(Font(FontOptions(14.5f, Font::bold)));
            g.drawText(items[(size_t) i].title, b.withTrimmedBottom(b.getHeight() * 0.48f).toNearestInt(), Justification::centred, true);
            g.setColour(Colour(0xff93949c).withMultipliedAlpha(a)); g.setFont(Font(FontOptions(11.5f)));
            g.drawText(items[(size_t) i].sub, b.withTrimmedTop(b.getHeight() * 0.50f).toNearestInt(), Justification::centred, true); } }
    void mouseMove(const MouseEvent& e) override { const int h = indexAt(e.getPosition()); if (h != hot) { hot = h; repaint(); } }
    void mouseExit(const MouseEvent&) override { hot = -1; repaint(); }
    void mouseDown(const MouseEvent& e) override { if (! isEnabled()) return; const int i = indexAt(e.getPosition()); if (i != sel) { sel = i; repaint(); if (onChange) onChange(i); } }
};
struct StemChip : Button {                                                                  
    Colour col;
    StemChip(const String& n, Colour c) : Button(n), col(c) { setClickingTogglesState(true); setMouseCursor(MouseCursor::PointingHandCursor); }
    void paintButton(Graphics& g, bool over, bool) override {
        const auto r = getLocalBounds().toFloat().reduced(1.f); const bool on = getToggleState(); const float cr = r.getHeight() * 0.5f, a = isEnabled() ? 1.f : 0.4f;
        g.setColour(on ? col.withAlpha(0.20f * a) : Colour(over && isEnabled() ? 0xff2d2e34 : 0xff23242a)); g.fillRoundedRectangle(r, cr);
        g.setColour(on ? col.withAlpha(0.9f * a) : Colour(0x24ffffff)); g.drawRoundedRectangle(r.reduced(0.5f), cr, 1.2f);
        const auto d = Rectangle<float>(r.getX() + 13.f, r.getCentreY() - 5.f, 10.f, 10.f);
        if (on) { g.setColour(col.withMultipliedAlpha(a)); g.fillEllipse(d); } else { g.setColour(Colour(0xff6d6e76).withMultipliedAlpha(a)); g.drawEllipse(d.reduced(0.7f), 1.5f); }
        g.setColour((on ? Colours::white : Colour(0xff9a9ba3)).withMultipliedAlpha(a)); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText(getButtonText(), r.withTrimmedLeft(32.f).toNearestInt(), Justification::centredLeft, true); }
};
struct StemChipGrid : Component {                                                           
    OwnedArray<StemChip> chips; std::function<void(StemChip*)> onToggle; int cols = 3;
    void add(const String& n, Colour c) { auto* ch = chips.add(new StemChip(n, c)); addAndMakeVisible(ch); ch->onClick = [this, ch] { if (onToggle) onToggle(ch); }; }
    void resized() override {
        const int n = chips.size(); if (n == 0) return; const int lines = (n + cols - 1) / cols, gap = 8; auto r = getLocalBounds();
        const int w = (r.getWidth() - gap * (cols - 1)) / cols, h = jmin(30, (r.getHeight() - gap * (lines - 1)) / lines), y0 = r.getY() + (r.getHeight() - (lines * h + gap * (lines - 1))) / 2;
        for (int i = 0; i < n; ++i) chips[i]->setBounds(r.getX() + (i % cols) * (w + gap), y0 + (i / cols) * (h + gap), w, h); }
};
struct StemHelpBubble : Component {                                                         
    TextLayout tl;
    StemHelpBubble(const String& title, const String& body, int width = 350) {
        AttributedString as; as.setJustification(Justification::topLeft); as.append(title + "\n", Font(FontOptions(14.f, Font::bold)), Colours::white); as.append("\n" + body, Font(FontOptions(12.5f)), Colour(0xffcfd0d6));
        tl.createLayout(as, (float) (width - 28)); setSize(width, (int) std::ceil(tl.getHeight()) + 28); }
    void paint(Graphics& g) override { g.fillAll(Colour(0xff2a2b31)); tl.draw(g, getLocalBounds().toFloat().reduced(14.f)); }
};
struct StemHelpButton : Component {                                                         
    String title, body; bool hot = false; std::function<std::unique_ptr<Component>()> make;       
    StemHelpButton(String t, String b) : title(std::move(t)), body(std::move(b)) { setMouseCursor(MouseCursor::PointingHandCursor); setSize(18, 18); }
    void paint(Graphics& g) override {
        const auto r = getLocalBounds().toFloat().reduced(1.5f); g.setColour(hot ? Colour(0x555b8def) : Colour(0x14ffffff)); g.fillEllipse(r);
        g.setColour(hot ? Colours::white : Colour(0xff9a9ba3)); g.drawEllipse(r, 1.2f); g.setFont(Font(FontOptions(12.f, Font::bold))); g.drawText("?", getLocalBounds(), Justification::centred); }
    void mouseEnter(const MouseEvent&) override { hot = true; repaint(); }
    void mouseExit(const MouseEvent&) override { hot = false; repaint(); }
    void mouseUp(const MouseEvent& e) override { if (! getLocalBounds().contains(e.getPosition())) return;
        if (make) CallOutBox::launchAsynchronously(make(), getScreenBounds(), nullptr); else CallOutBox::launchAsynchronously(std::make_unique<StemHelpBubble>(title, body), getScreenBounds(), nullptr); }
};
struct StemInstallHelp : Component {                                                         
    struct Block { String os; StringArray lines; Rectangle<int> head, code; };
    static constexpr int kW = 620;
    std::vector<Block> blocks; OwnedArray<TextButton> copyBtns; TextLayout intro, outro; Rectangle<int> introR, outroR;
    static Font mono() { return Font(FontOptions(Font::getDefaultMonospacedFontName(), 12.f, Font::plain)); }
    StemInstallHelp() {
        const String pipLine = "pip install \"torch<2.9\" \"torchaudio<2.9\" soundfile demucs", modelLine = "python -c \"from demucs.pretrained import get_model; get_model('htdemucs')\"";
        auto mk = [this](const String& os, std::initializer_list<String> l) { Block b; b.os = os; for (auto& x : l) b.lines.add(x); blocks.push_back(std::move(b)); };
        mk("macOS  (Terminal)", { "python3 -m venv ~/minidaw-demucs", "source ~/minidaw-demucs/bin/activate", pipLine, modelLine });
        mk("Windows  (Command Prompt)", { "py -3.12 -m venv %USERPROFILE%\\minidaw-demucs", "%USERPROFILE%\\minidaw-demucs\\Scripts\\activate", pipLine, modelLine });
        mk("Linux  (Terminal)", { "sudo apt install python3-venv   # Debian / Ubuntu only", "python3 -m venv ~/minidaw-demucs", "source ~/minidaw-demucs/bin/activate", pipLine, modelLine });
        AttributedString a; a.setJustification(Justification::topLeft);
        a.append("The Download button already does all of this for you: it sets Demucs up in a private folder and fetches the model. Use these commands only if you would rather install it yourself, or if the automatic install fails. You need Python 3.9 - 3.13 (Windows: install it from python.org first).", Font(FontOptions(12.5f)), Colour(0xffcfd0d6));
        intro.createLayout(a, (float) (kW - 28));
        int y = 14 + 26; introR = { 14, y, kW - 28, (int) std::ceil(intro.getHeight()) }; y += introR.getHeight() + 12;
        for (auto& b : blocks) {
            b.head = { 14, y, kW - 28, 24 }; y += 26; b.code = { 14, y, kW - 28, b.lines.size() * 17 + 14 }; y += b.code.getHeight() + 14;
            auto* cb = copyBtns.add(new TextButton("Copy")); addAndMakeVisible(cb); auto hb = b.head; cb->setBounds(hb.removeFromRight(64).withSizeKeepingCentre(64, 24));
            const String text = b.lines.joinIntoString("\n"); cb->onClick = [text, cb] { SystemClipboard::copyTextToClipboard(text); cb->setButtonText("Copied"); }; }
        AttributedString o; o.setJustification(Justification::topLeft);
        o.append("Afterwards reopen this window: MiniDAW finds the minidaw-demucs folder by itself. If you installed somewhere else, press Choose... next to Demucs and pick the demucs program (inside the environment's bin or Scripts folder).", Font(FontOptions(12.f)), Colour(0xff9a9ba3));
        outro.createLayout(o, (float) (kW - 28)); outroR = { 14, y, kW - 28, (int) std::ceil(outro.getHeight()) };
        setSize(kW, y + outroR.getHeight() + 14); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2a2b31)); g.setColour(Colours::white); g.setFont(Font(FontOptions(14.f, Font::bold))); g.drawText("Install Demucs manually (optional)", 14, 12, kW - 28, 22, Justification::centredLeft);
        intro.draw(g, introR.toFloat());
        for (auto& b : blocks) {
            g.setColour(Colour(0xffe9e9ee)); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText(b.os, b.head.withTrimmedRight(70), Justification::centredLeft);
            g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle(b.code.toFloat(), 8.f); g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(b.code.toFloat().reduced(0.5f), 8.f, 1.f);
            g.setColour(Colour(0xffd6d7dd)); g.setFont(mono());
            for (int i = 0; i < b.lines.size(); ++i) g.drawText(b.lines[i], b.code.getX() + 10, b.code.getY() + 7 + i * 17, b.code.getWidth() - 20, 17, Justification::centredLeft, false); }
        outro.draw(g, outroR.toFloat()); }
};
struct StemModelState { bool installed = false, downloading = false; float progress = 0.f; String status; };
struct StemModelPicker : Component {                                                        
    int idx = 0; bool installed = false, hot = false; std::function<void()> onClick;
    void paint(Graphics& g) override {
        const float a = isEnabled() ? 1.f : 0.5f; const auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(Colour(hot && isEnabled() ? 0xff34353c : 0xff26272c)); g.fillRoundedRectangle(r, 8.f); g.setColour(Colour(0x24ffffff)); g.drawRoundedRectangle(r, 8.f, 1.f);
        g.setColour((installed ? Colour(0xff3ecf8e) : Colour(0xffe0b341)).withMultipliedAlpha(a)); g.fillEllipse(r.getX() + 11.f, r.getCentreY() - 4.f, 8.f, 8.f);
        g.setColour(Colours::white.withMultipliedAlpha(a)); g.setFont(Font(FontOptions(13.5f, Font::bold))); g.drawText(stemui::kModelInfo[idx].key, getLocalBounds().withTrimmedLeft(28).withTrimmedRight(120), Justification::centredLeft, true);
        g.setColour((installed ? Colour(0xff3ecf8e) : Colour(0xffe0b341)).withMultipliedAlpha(a)); g.setFont(Font(FontOptions(11.5f)));
        g.drawText(installed ? "Ready" : "Download needed", getLocalBounds().withTrimmedRight(28).removeFromRight(110), Justification::centredRight, true);
        Path p; const float cx = r.getRight() - 15.f, cy = r.getCentreY(); p.startNewSubPath(cx - 4.f, cy - 2.f); p.lineTo(cx, cy + 2.f); p.lineTo(cx + 4.f, cy - 2.f);
        g.setColour(Colour(0xffa6a7af).withMultipliedAlpha(a)); g.strokePath(p, PathStrokeType(1.6f)); }
    void mouseEnter(const MouseEvent&) override { hot = true; repaint(); }
    void mouseExit(const MouseEvent&) override { hot = false; repaint(); }
    void mouseDown(const MouseEvent&) override { if (isEnabled() && onClick) onClick(); }
};
struct StemModelList : Component, Timer {                                                   
    std::function<StemModelState(int)> getState; std::function<int()> getCurrent; std::function<void(int)> onPick, onDownload; std::function<void()> onCancel;
    OwnedArray<TextButton> btns; static constexpr int kRowH = 66, kHead = 34, kW = 470; int hotRow = -1; StemHelpButton manualHelp { "", "" };
    StemModelList() {
        for (int i = 0; i < stemui::kNumModels; ++i) { auto* b = btns.add(new TextButton("Download")); addAndMakeVisible(b);
            b->onClick = [this, i] { const auto st = getState ? getState(i) : StemModelState(); if (st.downloading) { if (onCancel) onCancel(); } else if (onDownload) onDownload(i); }; }
        manualHelp.make = [] { return std::make_unique<StemInstallHelp>(); }; addAndMakeVisible(manualHelp);
        setSize(kW, kHead + kRowH * stemui::kNumModels + 8); startTimerHz(8); }
    Rectangle<int> rowR(int i) const { return { 0, kHead + i * kRowH, getWidth(), kRowH }; }
    void resized() override { for (int i = 0; i < btns.size(); ++i) btns[i]->setBounds(rowR(i).withTrimmedRight(14).removeFromRight(96).withSizeKeepingCentre(96, 28)); manualHelp.setCentrePosition(getWidth() - 24, kHead / 2); }
    void refresh() {
        bool anyDl = false; for (int i = 0; i < stemui::kNumModels; ++i) if (getState && getState(i).downloading) anyDl = true;
        for (int i = 0; i < stemui::kNumModels; ++i) { const auto st = getState ? getState(i) : StemModelState(); auto* b = btns[i]; b->setVisible(! st.installed); b->setButtonText(st.downloading ? "Cancel" : "Download"); b->setEnabled(st.downloading || ! anyDl); }
        repaint(); }
    void timerCallback() override { refresh(); }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff2a2b31)); g.setColour(Colour(0xffa6a7af)); g.setFont(Font(FontOptions(12.f)));
        g.drawText("Pick a model. Missing ones download automatically.", 14, 6, getWidth() - 28 - 120, kHead - 10, Justification::centredLeft, true);
        g.drawText("Manual install", getWidth() - 33 - 96, 6, 92, kHead - 10, Justification::centredRight, true);
        const int cur = getCurrent ? getCurrent() : -1;
        for (int i = 0; i < stemui::kNumModels; ++i) {
            const auto r = rowR(i); const auto st = getState ? getState(i) : StemModelState(); const float y = (float) r.getY(), w = (float) getWidth();
            if (i == cur) { g.setColour(Colour(0x225b8def)); g.fillRoundedRectangle(r.toFloat().reduced(6.f, 3.f), 8.f); } else if (i == hotRow) { g.setColour(Colour(0x0fffffff)); g.fillRoundedRectangle(r.toFloat().reduced(6.f, 3.f), 8.f); }
            if (i > 0) { g.setColour(Colour(0x12ffffff)); g.drawHorizontalLine(r.getY(), 14.f, w - 14.f); }
            g.setColour(st.installed ? Colour(0xff3ecf8e) : Colour(0xffe0b341)); g.fillEllipse(16.f, y + 14.f, 8.f, 8.f);
            g.setColour(Colours::white); g.setFont(Font(FontOptions(13.5f, Font::bold))); g.drawText(stemui::kModelInfo[i].key, 32, r.getY() + 8, 260, 18, Justification::centredLeft, true);
            g.setColour(Colour(0xff93949c)); g.setFont(Font(FontOptions(11.5f))); g.drawText(st.downloading && st.status.isNotEmpty() ? st.status : String(stemui::kModelInfo[i].sub), 32, r.getY() + 27, getWidth() - 32 - 122, 16, Justification::centredLeft, true);
            if (st.downloading) { const auto bar = Rectangle<float>(32.f, y + 48.f, w - 32.f - 122.f, 6.f); g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle(bar, 3.f);
                g.setColour(Colour(0xff5b8def)); g.fillRoundedRectangle(bar.withWidth(jmax(6.f, bar.getWidth() * jlimit(0.f, 1.f, st.progress))), 3.f);
                g.setColour(Colour(0xffd6d7dd)); g.setFont(Font(FontOptions(11.f))); g.drawText(String(roundToInt(st.progress * 100.f)) + "%", (int) w - 14 - 96, r.getY() + 42, 96, 16, Justification::centred); }
            else if (st.installed) { g.setColour(Colour(0xff3ecf8e)); g.setFont(Font(FontOptions(12.f, Font::bold))); g.drawText("Ready", (int) w - 14 - 96, r.getY() + (kRowH - 20) / 2, 96, 20, Justification::centred); } } }
    int rowAt(Point<int> p) const { for (int i = 0; i < stemui::kNumModels; ++i) if (rowR(i).contains(p) && ! (btns[i]->isVisible() && btns[i]->getBounds().contains(p))) return i; return -1; }
    void mouseMove(const MouseEvent& e) override { const int h = rowAt(e.getPosition()); if (h != hotRow) { hotRow = h; repaint(); } }
    void mouseExit(const MouseEvent&) override { hotRow = -1; repaint(); }
    void mouseUp(const MouseEvent& e) override {
        const int i = rowAt(e.getPosition()); if (i < 0) return; if (onPick) onPick(i);
        Component::SafePointer<CallOutBox> cb(findParentComponentOfClass<CallOutBox>()); MessageManager::callAsync([cb] { if (cb != nullptr) cb->dismiss(); }); }
};
struct StemExeBox : Component {                                                             
    int state = 0; String path; TextButton choose { "Choose..." };                           
    StemExeBox() { addAndMakeVisible(choose); }
    void resized() override { choose.setBounds(getLocalBounds().removeFromRight(92).withSizeKeepingCentre(92, 28)); }
    void paint(Graphics& g) override {
        const auto r = getLocalBounds().withTrimmedRight(100); const Colour c = state == 1 ? Colour(0xff3ecf8e) : state == 2 ? Colour(0xffe5645a) : Colour(0xffe0b341);
        g.setColour(c); g.fillEllipse((float) r.getX() + 1.f, (float) r.getCentreY() - 4.f, 8.f, 8.f);
        g.setColour(Colour(0xffd6d7dd)); g.setFont(Font(FontOptions(13.f)));
        g.drawText(state == 0 ? String("Looking for Demucs...") : state == 1 ? path : String("Not installed yet - press Download and MiniDAW sets it up"), r.withTrimmedLeft(16), Justification::centredLeft, true); }
};
struct StemBody : Component {                                                               
    std::function<void(Graphics&)> onPaint; void paint(Graphics& g) override { if (onPaint) onPaint(g); } };
struct StemSplitterPanel : Component, Thread, Timer {
    enum State { Idle, Running, Done, Failed };
    std::function<void(std::vector<StemOut>&)> onDone; std::function<void(const StemSettings&)> onChanged;
    StemSettings cfg;
    StemSplitterPanel(const String& clipName, std::shared_ptr<std::vector<float>> wavData, double sampleRate, std::vector<float> peakData, const StemSettings& s)
        : Thread("MiniDAW stem splitter"), cfg(s), name(clipName), src(std::move(wavData)), srcSr(sampleRate), peaks(std::move(peakData)) {
        engine.items = { { "Quick Split", "Built-in  -  6 stems by pitch range  -  no setup" }, { "AI Split (Demucs)", "Best quality  -  real instrument separation" } };
        engine.sel = cfg.engine; engine.onChange = [this](int i) { cfg.engine = i; ensureModelForStems(); refreshLayout(); changed(); }; addAndMakeVisible(engine);
        for (int i = 0; i < stemui::kNumStems; ++i) {
            quickChips.add(stemui::nameFor(stemui::kKeys[i]), stemui::colourFor(stemui::kKeys[i])); aiChips.add(stemui::nameFor(stemui::kKeys[i]), stemui::colourFor(stemui::kKeys[i]));
            quickChips.chips[i]->setToggleState(((cfg.qMask >> i) & 1) != 0, dontSendNotification); aiChips.chips[i]->setToggleState(((cfg.aMask >> i) & 1) != 0, dontSendNotification); }
        quickChips.onToggle = [this](StemChip* c) { keepOne(quickChips, c); cfg.qMask = maskOf(quickChips); repaint(); changed(); };
        aiChips.onToggle = [this](StemChip* c) { keepOne(aiChips, c); cfg.aMask = maskOf(aiChips); ensureModelForStems(); repaint(); changed(); };
        quality.addItemList({ "Fast", "Balanced", "Best (slower)" }, 1); quality.setSelectedItemIndex(cfg.quality, dontSendNotification); quality.onChange = [this] { cfg.quality = quality.getSelectedItemIndex(); changed(); };
        setupSlider(sharp, 1.0, 4.0, 0.1, cfg.sharpness, "", 1, [this] { cfg.sharpness = (float) sharp.getValue(); });
        setupSlider(sustain, 60.0, 500.0, 10.0, cfg.sustainMs, " ms", 0, [this] { cfg.sustainMs = (float) sustain.getValue(); });
        setupSlider(drumW, 200.0, 2000.0, 50.0, cfg.drumWidthHz, " Hz", 0, [this] { cfg.drumWidthHz = (float) drumW.getValue(); });
        setupSlider(bassHz, 60.0, 400.0, 5.0, cfg.bassHz, " Hz", 0, [this] { cfg.bassHz = (float) bassHz.getValue(); });
        setupSlider(gTop, 250.0, 1000.0, 10.0, cfg.guitarTopHz, " Hz", 0, [this] { cfg.guitarTopHz = (float) gTop.getValue(); });
        setupSlider(pTop, 600.0, 3000.0, 50.0, cfg.pianoTopHz, " Hz", 0, [this] { cfg.pianoTopHz = (float) pTop.getValue(); });
        setupSlider(vTop, 2000.0, 8000.0, 100.0, cfg.vocalTopHz, " Hz", 0, [this] { cfg.vocalTopHz = (float) vTop.getValue(); });
        modelPick.idx = cfg.model; modelPick.installed = isInstalled(cfg.model); modelPick.onClick = [this] { openModelList(); };
        setupSlider(shifts, 0.0, 5.0, 1.0, cfg.shifts, "", 0, [this] { cfg.shifts = (int) shifts.getValue(); });
        setupSlider(overlap, 0.10, 0.50, 0.05, cfg.overlap, "", 2, [this] { cfg.overlap = (float) overlap.getValue(); });
        device.addItemList({ "Automatic", "CPU only", "Apple GPU (experimental)", "NVIDIA GPU (CUDA)" }, 1); device.setSelectedItemIndex(cfg.device, dontSendNotification); device.onChange = [this] { cfg.device = device.getSelectedItemIndex(); changed(); };
        for (auto* cb : { &quality, &device }) cb->setScrollWheelEnabled(false);               
        exeBox.choose.onClick = [this] { chooseExe(); };
        cfg.mode = 0; cfg.instrumental = false;                                                  
        toggleMute.setToggleState(cfg.muteOriginal, dontSendNotification); toggleMute.onClick = [this] { cfg.muteOriginal = toggleMute.getToggleState(); changed(); };
        toggleSave.setToggleState(cfg.saveFiles, dontSendNotification); toggleSave.onClick = [this] { cfg.saveFiles = toggleSave.getToggleState(); changed(); };
        addRow("Stems", "Ticked parts get a track each; the rest share one.", &quickChips, 0, 84,
               "Tick the parts you want. Each ticked part becomes its own track, and everything you did not tick is kept together in one extra \"Rest\" track - so nothing is lost and all the tracks add back up to the original sound.\n\nHow it works: drum hits are separated from sustained notes, then the sustained sound is sorted by pitch range - Bass is the lowest, then Guitar, Piano and Vocals, and Other is the high end that is left.\n\nGuitar - sustained sound in the low-mid range.\nPiano - sustained sound in the mid range.\nBass - sustained low notes.\nVocals - sustained sound in the upper-mid range where voices sit.\nDrums - hits at any pitch (kick, snare, hats).\nOther - the high end that remains (air, shimmer, cymbal tails).\n\nThis is a fast approximation: a guitar and a piano playing in the same range land in the same stem. For true instrument separation use AI Split.");
        addRow("Quality", "Higher is cleaner but takes longer.", &quality, 0, 42,
               "Sets the size of the analysis window.\n\nFast - quickest, less exact on low notes.\nBalanced - good for most music.\nBest - sharpest pitch separation and cleanest bass, but slower, and drum hits get slightly softer.");
        addRow("Separation strength", "Soft blends naturally, hard isolates more.", &sharp, 0, 42,
               "How strictly each sound is given to one stem.\n\nLow (1) - soft and natural, but stems overlap more.\nHigh (4) - cleaner, more isolated stems, but they can sound thin or watery.\n\nStart around 2.");
        addRow("Sustain smoothing", "How long a sound must last to count as a note.", &sustain, 0, 42,
               "How long a sound has to last to count as a held note instead of a drum hit.\n\nLonger - only long notes go to the pitched stems, more is treated as Drums.\nShorter - quick plucks and stabs also count as pitched sound.");
        addRow("Drum width", "How broad a hit must be to count as drums.", &drumW, 0, 42,
               "How wide a burst of sound must be across the frequency range to count as a drum hit.\n\nWider - only broad hits like snares and cymbals go to Drums.\nNarrower - also catches kicks and toms, but more instrument attacks leak into Drums.");
        addRow("Bass / Guitar split", "Sustained sound below this pitch is Bass.", &bassHz, 0, 42,
               "The pitch where Bass ends and Guitar begins. Raise it if the upper notes of a bass guitar leak into Guitar; lower it for a tighter low end.\n\nThe edge is blended smoothly (about a third of an octave) so nothing is lost.");
        addRow("Guitar / Piano split", "Where Guitar ends and Piano begins.", &gTop, 0, 42,
               "The pitch where Guitar ends and Piano begins. Sound between the Bass split and this value goes to Guitar.\n\nIf you set the splits out of order they are nudged apart automatically.");
        addRow("Piano / Vocals split", "Where Piano ends and Vocals begin.", &pTop, 0, 42,
               "The pitch where Piano ends and Vocals begin. Sound between the Guitar split and this value goes to Piano.\n\nLower it to give the singer more of the upper mids; raise it to give the piano more.");
        addRow("Vocals / Other split", "Where Vocals end and Other begins.", &vTop, 0, 42,
               "The pitch where Vocals end and Other begins. Everything above this goes to Other - the airy top end, cymbal tails and sparkle.");
        addRow("Demucs", "Runs on this computer. Set up automatically.", &exeBox, 1, 42,
               "Demucs is a free, open-source AI tool for separating music. MiniDAW runs it on this computer - your audio is never uploaded.");
        rows.back().help->make = [] { return std::make_unique<StemInstallHelp>(); };                    
        { String m = "Each model is a different neural network. Pick one from the drop-down. Models that are not on this computer have a Download button: it downloads the model (and sets Demucs up first if it is not installed yet) and selects it right away. If you start a split with a model that is not downloaded yet, MiniDAW downloads it first.\n\nGuitar and Piano only exist in htdemucs_6s, so ticking either one switches to that model.\n";
          for (int i = 0; i < stemui::kNumModels; ++i) m += "\n" + String(stemui::kModelInfo[i].key) + " - " + stemui::kModelInfo[i].help + (stemui::kModelInfo[i].files > 1 ? " (" + String(stemui::kModelInfo[i].files) + " files)" : String());
          addRow("Model", stemui::kModelInfo[cfg.model].sub, &modelPick, 1, 42, m); modelRow = (int) rows.size() - 1; }
        addRow("Stems", "Ticked parts get a track each; the rest share one.", &aiChips, 1, 84,
               "Tick the parts you want. Each ticked part becomes its own track, and everything you did not tick is mixed together into one extra \"Rest\" track.\n\nExample: tick only Vocals and you get two tracks - Vocals and Rest (a backing track). Tick Vocals and Drums and you get Vocals, Drums and Rest.\n\nGuitar and Piano only exist in the 6-stem model, so ticking either one switches the model to htdemucs_6s. Picking a 4-stem model unticks them again.");
        addRow("Random shifts", "More passes are cleaner but proportionally slower.", &shifts, 1, 42,
               "Demucs separates the audio several times, slightly shifted each time, and averages the results. Each extra shift reduces artefacts but costs another full pass.\n\n0 - fastest.\n1 - good default.\n2 to 5 - best quality, slow.");
        addRow("Chunk overlap", "Higher hides seams between processed chunks.", &overlap, 1, 42,
               "Long audio is processed in chunks. Overlap is how much neighbouring chunks share, which hides the seams between them.\n\nHigher hides seams better but is slower. 0.25 is a good default.");
        addRow("Device", "Auto picks your GPU if Demucs can use one.", &device, 1, 42,
               "Which part of the computer does the work.\n\nAutomatic - Demucs decides.\nCPU only - always works, slowest.\nApple GPU - much faster on Apple Silicon Macs, but experimental.\nNVIDIA GPU (CUDA) - fastest on PCs with an NVIDIA card.");
        addRow("Mute original track", "Silences the source so you hear only the stems.", &toggleMute, 2, 42,
               "After splitting, mutes the original region so you hear only the new stems. You can unmute it any time.");
        addRow("Save stem files", "Also write WAV files to Music / MiniDAW Stems.", &toggleSave, 2, 42,
               "Also writes each stem as a WAV file to Music / MiniDAW Stems / (clip name), so you can use them outside MiniDAW.");
        body.onPaint = [this](Graphics& g) { paintBody(g); }; vp.setViewedComponent(&body, false); vp.setScrollBarsShown(true, false); vp.setScrollBarThickness(9); addAndMakeVisible(vp);
        go.onClick = [this] { goPressed(); }; addAndMakeVisible(go); reveal.onClick = [this] { if (savedDir.isDirectory()) savedDir.revealToUser(); }; addChildComponent(reveal);
        lookAndFeelChanged(); ensureModelForStems(); syncAiChips(); refreshLayout(); detectExe(); startTimerHz(30);
    }
    ~StemSplitterPanel() override { stopTimer(); cancelFlag = true; killProcess(); if (dl) dl->killNow(); stopThread(10000); }
    struct Row { String label, desc; Component* c; int group; int h; StemHelpButton* help; };
    std::vector<Row> rows; int modelRow = -1;
    Rectangle<int> hdr, engR, foot, setCard, outCard; int setTitleY = 0, outTitleY = 0;
    Colour acc() const { return getLookAndFeel().findColour(Slider::trackColourId); }
    void lookAndFeelChanged() override { go.setColour(TextButton::buttonColourId, acc()); go.setColour(TextButton::buttonOnColourId, acc()); repaint(); }
    void addRow(const String& label, const String& desc, Component* c, int group, int h, const String& help) {
        rows.push_back({ label, desc, c, group, h, nullptr }); body.addChildComponent(*c);
        if (help.isNotEmpty()) { auto* hb = helpBtns.add(new StemHelpButton(label, help)); body.addChildComponent(hb); rows.back().help = hb; } }
    bool rowShown(const Row& r) const { return r.group == 2 || (r.group == 3 && cfg.engine == 1) || r.group == cfg.engine; }
    static bool isOut(const Row& r) { return r.group >= 2; }
    void refreshLayout() { for (auto& r : rows) { const bool v = rowShown(r); r.c->setVisible(v); if (r.help != nullptr) r.help->setVisible(v); } vp.setViewPosition(0, 0); updateGo(); resized(); repaint(); body.repaint(); }
    void resized() override {
        auto r = getLocalBounds().reduced(22, 18); hdr = r.removeFromTop(78); r.removeFromTop(12); engR = r.removeFromTop(58); r.removeFromTop(12); foot = r.removeFromBottom(52); r.removeFromBottom(10);
        engine.setBounds(engR); vp.setBounds(r); layoutBody();
        auto f = foot; go.setBounds(f.removeFromRight(170).withSizeKeepingCentre(170, 40)); f.removeFromRight(10); if (reveal.isVisible() || state == Done) reveal.setBounds(f.removeFromRight(120).withSizeKeepingCentre(120, 34)); }
    void layoutBody() {
        const int W = jmax(300, vp.getMaximumVisibleWidth()); int y = 0, hS = 0, hO = 0;
        for (auto& r : rows) if (rowShown(r)) (isOut(r) ? hO : hS) += r.h;
        setTitleY = y; y += 22; setCard = { 0, y, W, hS }; y += hS + 14; outTitleY = y; y += 22; outCard = { 0, y, W, hO }; y += hO + 14;
        int ys = setCard.getY(), yo = outCard.getY();
        for (auto& row : rows) { if (! rowShown(row)) continue; const bool out = isOut(row); const auto& card = out ? outCard : setCard; int& yy = out ? yo : ys;
            auto cell = Rectangle<int>(card.getX() + 270, yy, card.getWidth() - 270 - 14, row.h).reduced(0, row.h > 50 ? 8 : 6);
            if (dynamic_cast<ToggleButton*>(row.c) != nullptr) cell = cell.removeFromRight(48).withSizeKeepingCentre(46, 28);
            row.c->setBounds(cell);
            if (row.help != nullptr) { const int tw = (int) std::ceil(stemui::textW(Font(FontOptions(13.5f, Font::bold)), row.label)); row.help->setCentrePosition(card.getX() + 16 + tw + 16, yy + 14); }
            yy += row.h; }
        body.setSize(W, y); }
    void paintBody(Graphics& g) {
        auto caption = [&](const String& s, int y) { g.setColour(Colour(0xff8d8e96)); g.setFont(Font(FontOptions(11.f, Font::bold))); g.drawText(s.toUpperCase(), setCard.getX() + 4, y, 300, 20, Justification::centredLeft); };
        caption(cfg.engine == 0 ? "Quick Split settings" : "AI Split settings", setTitleY); caption("Output", outTitleY);
        for (auto* card : { &setCard, &outCard }) { const auto c = card->toFloat(); g.setColour(Colour(0xff2d2e34)); g.fillRoundedRectangle(c, 12.f); g.setColour(Colour(0x12ffffff)); g.drawRoundedRectangle(c.reduced(0.5f), 12.f, 1.f); }
        int ys = setCard.getY(), yo = outCard.getY(); bool firstS = true, firstO = true;
        for (auto& row : rows) { if (! rowShown(row)) continue; const bool out = isOut(row); int& y = out ? yo : ys; bool& first = out ? firstO : firstS; const auto& card = out ? outCard : setCard;
            if (! first) { g.setColour(Colour(0x12ffffff)); g.drawHorizontalLine(y, (float) card.getX() + 16.f, (float) card.getRight() - 16.f); } first = false;
            g.setColour(Colour(0xffe9e9ee)); g.setFont(Font(FontOptions(13.5f, Font::bold))); g.drawText(row.label, card.getX() + 16, y + 5, 250, 18, Justification::centredLeft, false);
            g.setColour(Colour(0xff8d8e96)); g.setFont(Font(FontOptions(11.f))); g.drawText(row.desc, card.getX() + 16, y + 22, 252, 15, Justification::centredLeft, true); y += row.h; } }
    void paint(Graphics& g) override {
        const Colour a = acc(); g.setGradientFill(ColourGradient(Colour(0xff2a2b31), 0.f, 0.f, Colour(0xff202125), 0.f, (float) getHeight(), false)); g.fillAll();
        { const auto h = hdr.toFloat(); g.setColour(Colour(0xff1b1c20)); g.fillRoundedRectangle(h, 14.f);
          if (! peaks.empty()) { const int cols = jmax(1, (int) (h.getWidth() / 4.f)); const float mid = h.getCentreY(), amp = h.getHeight() * 0.40f;
              for (int i = 0; i < cols; ++i) { const size_t i0 = (size_t) ((double) i / cols * (double) peaks.size()), i1 = jmax(i0 + 1, (size_t) ((double) (i + 1) / cols * (double) peaks.size())); float pk = 0.f;
                  for (size_t k = i0; k < jmin(i1, peaks.size()); ++k) { pk = jmax(pk, peaks[k]); }
                  const float hh = jmax(1.5f, jmin(1.f, pk) * amp); const float x = h.getX() + 6.f + (float) i * (h.getWidth() - 12.f) / (float) cols;
                  g.setColour(a.withAlpha(0.22f + 0.28f * pk)); g.fillRoundedRectangle(x, mid - hh, 2.4f, hh * 2.f, 1.2f); } }
          g.setGradientFill(ColourGradient(Colour(0xf21b1c20), h.getX(), 0.f, Colour(0x001b1c20), h.getX() + h.getWidth() * 0.62f, 0.f, false)); g.fillRoundedRectangle(h, 14.f);
          g.setColour(Colour(0x14ffffff)); g.drawRoundedRectangle(h.reduced(0.5f), 14.f, 1.f);
          const double secs = srcSr > 0 ? (double) src->size() / srcSr : 0.0; const String info = String((int) secs / 60) + ":" + String((int) secs % 60).paddedLeft('0', 2) + "   -   " + String(srcSr / 1000.0, 1) + " kHz   -   mono";
          auto t = hdr.reduced(18, 0); g.setColour(Colours::white); g.setFont(Font(FontOptions(19.f, Font::bold))); g.drawText(name, t.removeFromTop(hdr.getHeight() / 2 + 6).withTrimmedTop(10), Justification::bottomLeft, true);
          g.setColour(Colour(0xffa6a7af)); g.setFont(Font(FontOptions(12.5f))); g.drawText(info, t.withTrimmedTop(2), Justification::topLeft, true); }
        auto f = foot.withTrimmedRight(go.getWidth() + 10 + (reveal.isVisible() ? reveal.getWidth() + 10 : 0)).toFloat();
        if (state == Running) { const float p = jlimit(0.f, 1.f, progress.load()); g.setColour(Colour(0xffd6d7dd)); g.setFont(Font(FontOptions(13.f, Font::bold))); g.drawText(getStatus(), f.withHeight(20.f).toNearestInt(), Justification::centredLeft, true);
            g.setColour(Colour(0xff93949c)); g.drawText(String(roundToInt(p * 100.f)) + "%", f.withHeight(20.f).toNearestInt(), Justification::centredRight, true);
            const auto bar = Rectangle<float>(f.getX(), f.getY() + 26.f, f.getWidth(), 8.f); g.setColour(Colour(0xff17181b)); g.fillRoundedRectangle(bar, 4.f);
            if (p > 0.f) { auto fill = bar.withWidth(jmax(8.f, bar.getWidth() * p)); g.setGradientFill(ColourGradient(a.brighter(0.2f), fill.getX(), 0.f, a, fill.getRight(), 0.f, false)); g.fillRoundedRectangle(fill, 4.f); } }
        else if (state == Done || state == Failed) { const bool ok = state == Done; g.setColour(ok ? Colour(0xff3ecf8e) : Colour(0xffe5645a)); g.fillEllipse(f.getX(), f.getCentreY() - 5.f, 10.f, 10.f);
            g.setColour(Colour(0xffe4e4e8)); g.setFont(Font(FontOptions(13.5f, Font::bold))); g.drawText(getStatus(), f.withTrimmedLeft(18.f).toNearestInt(), Justification::centredLeft, true); }
        else { g.setColour(Colour(0xff8d8e96)); g.setFont(Font(FontOptions(12.f))); const auto pl = planned(); float x = f.getX(); const float cy = f.getCentreY();
            const String lead = "Creates " + String((int) pl.size()) + (pl.size() == 1 ? " track:" : " tracks:"); const float lw = stemui::textW(Font(FontOptions(12.f)), lead); g.drawText(lead, (int) x, (int) cy - 10, (int) lw + 8, 20, Justification::centredLeft); x += lw + 14.f;
            for (auto& s : pl) { const float tw = stemui::textW(Font(FontOptions(12.5f, Font::bold)), s.first); if (x + tw + 26.f > f.getRight()) break; g.setColour(s.second); g.fillEllipse(x, cy - 4.f, 8.f, 8.f); g.setColour(Colour(0xffd6d7dd)); g.setFont(Font(FontOptions(12.5f, Font::bold))); g.drawText(s.first, (int) x + 13, (int) cy - 10, (int) tw + 6, 20, Justification::centredLeft); x += tw + 28.f; } } }
    std::vector<std::pair<String, Colour>> planned() const {                                  
        std::vector<std::pair<String, Colour>> v; auto add = [&v](const char* k) { v.emplace_back(stemui::nameFor(k), stemui::colourFor(k)); };
        const int m = (cfg.engine == 0 ? cfg.qMask : cfg.aMask) & 63;
        for (int i = 0; i < stemui::kNumStems; ++i) if ((m >> i) & 1) add(stemui::kKeys[i]);
        if (m != 63) add("rest");                                                              
        return v; }
    void changed() { if (onChanged) onChanged(cfg); }
    static int maskOf(const StemChipGrid& g) { int m = 0; for (int i = 0; i < g.chips.size(); ++i) if (g.chips[i]->getToggleState()) m |= 1 << i; return m; }
    static void keepOne(StemChipGrid& g, StemChip* c) { for (auto* ch : g.chips) if (ch->isEnabled() && ch->getToggleState()) return; c->setToggleState(true, dontSendNotification); }   
    bool isInstalled(int m) const { return ((cfg.installed >> m) & 1) != 0; }
    void syncAiChips() { aiChips.repaint(); }
    void modelChanged() { if (modelRow >= 0) rows[(size_t) modelRow].desc = stemui::kModelInfo[cfg.model].sub; syncAiChips(); updateGo(); repaint(); body.repaint(); }
    void updateGo() { const bool needsDl = cfg.engine == 1 && (! isInstalled(cfg.model) || exeBox.state == 2);
        go.setButtonText(state == Running ? "Cancel" : state == Done ? "Close" : state == Failed ? "Try Again" : needsDl ? "Download and Split" : "Split into Stems"); }
    void setupSlider(Slider& s, double lo, double hi, double step, double v, const String& unit, int dp, std::function<void()> apply) {
        s.setSliderStyle(Slider::LinearHorizontal); s.setTextBoxStyle(Slider::TextBoxRight, false, 78, 22); s.setRange(lo, hi, step); s.setValue(v, dontSendNotification); s.setScrollWheelEnabled(false);
        s.textFromValueFunction = [unit, dp](double x) { return String(x, dp) + unit; }; s.valueFromTextFunction = [](const String& t) { return t.getDoubleValue(); }; s.updateText();
        s.onValueChange = [this, apply] { apply(); changed(); }; }
    void setStatus(const String& s) { const ScopedLock l(statusLock); status = s; }
    String getStatus() const { const ScopedLock l(statusLock); return status; }
    void killProcess() { const ScopedLock l(procLock); if (active != nullptr) active->kill(); }
    void timerCallback() override { if (state == Running) { if (dlMode && dl) { progress = dl->progress.load(); setStatus(dl->getStatus()); } repaint(foot); } }
    void detectExe() {
        exeBox.state = 0; exeBox.repaint(); const String user = cfg.exe; Component::SafePointer<StemSplitterPanel> sp(this);
        Thread::launch([sp, user] { const File f = stemui::findDemucs(user); MessageManager::callAsync([sp, f] { if (sp != nullptr) sp->exeResolved(f); }); }); }
    void exeResolved(const File& f) { exeFile = f; exeBox.state = f.existsAsFile() ? 1 : 2; exeBox.path = f.getFullPathName(); exeBox.repaint(); updateGo(); }
    void chooseExe() {
        chooser = std::make_unique<FileChooser>("Choose the demucs program", File("/opt/homebrew/bin"), "*");
        chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this](const FileChooser& fc) { const auto f = fc.getResult(); if (f == File()) return; cfg.exe = f.getFullPathName(); changed(); detectExe(); }); }
    void ensureModelForStems() { if (cfg.engine == 1 && (cfg.aMask & 3) != 0 && cfg.model != 2) pickModel(2); }     
    void pickModel(int i) {
        cfg.model = jlimit(0, stemui::kNumModels - 1, i);
        if (cfg.model != 2 && (cfg.aMask & 3) != 0) {                                          
            int m = cfg.aMask & ~3; if (m == 0) m = 60; cfg.aMask = m;
            for (int k = 0; k < aiChips.chips.size(); ++k) aiChips.chips[k]->setToggleState(((m >> k) & 1) != 0, dontSendNotification);
            aiChips.repaint(); }
        modelPick.idx = cfg.model; modelPick.installed = isInstalled(cfg.model); modelPick.repaint(); modelChanged(); changed(); }
    void openModelList() {
        auto list = std::make_unique<StemModelList>(); Component::SafePointer<StemSplitterPanel> sp(this);
        list->getState = [sp](int i) -> StemModelState { StemModelState s; if (sp == nullptr) return s; s.installed = sp->isInstalled(i);
            if (sp->state == Running && sp->dlMode && sp->dlModel == i) { s.downloading = true; if (sp->dl) { s.progress = sp->dl->progress.load(); s.status = sp->dl->getStatus(); } } return s; };
        list->getCurrent = [sp] { return sp != nullptr ? sp->cfg.model : 0; };
        list->onPick = [sp](int i) { if (sp != nullptr) sp->pickModel(i); };
        list->onDownload = [sp](int i) { if (sp != nullptr) sp->startDownload(i, false); };
        list->onCancel = [sp] { if (sp != nullptr && sp->dl) sp->dl->killNow(); };
        picker = &CallOutBox::launchAsynchronously(std::move(list), modelPick.getScreenBounds(), nullptr); }
    void startDownload(int m, bool thenSplit) {                                                 
        if (state == Running) return;
        dl = std::make_shared<stemui::DlShared>(); dlModel = m; dlThenSplit = thenSplit; dlMode = true; cancelFlag = false; progress = 0.02f; setStatus("Starting download..."); state = Running;
        reveal.setVisible(false); setBusy(true); resized(); repaint();
        auto sh = dl; const String user = cfg.exe; Component::SafePointer<StemSplitterPanel> sp(this);
        Thread::launch([sh, user, m, sp] { String err; File exe; const bool ok = stemui::downloadModel(sh, user, m, err, exe); const bool cancelled = sh->cancel.load(); const String exePath = exe.getFullPathName();
            MessageManager::callAsync([sp, ok, cancelled, err, m, exePath] { if (sp != nullptr) sp->downloadFinished(ok, cancelled, err, m, exePath); }); }); }
    void downloadFinished(bool ok, bool cancelled, const String& err, int m, const String& exePath) {
        const bool thenSplit = dlThenSplit; dl.reset(); dlMode = false; state = Idle; setStatus({}); setBusy(false);
        if (exePath.isNotEmpty() && File(exePath).existsAsFile()) exeResolved(File(exePath));
        if (ok) { cfg.installed |= 1 << m; pickModel(m); if (picker != nullptr) picker->dismiss(); if (thenSplit && cfg.engine == 1) { beginSplit(); return; } resized(); repaint(); return; }
        updateGo(); resized(); repaint();
        if (! cancelled) AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Model download", "Could not set up " + String(stemui::kModelInfo[m].key) + ".\n\n" + err + "\n\nCheck your internet connection and try again. If it keeps failing, click the ? next to Demucs for manual install commands."); }
    void goPressed() {
        if (state == Running) { cancelFlag = true; setStatus("Cancelling..."); killProcess(); if (dl) dl->killNow(); return; }
        if (state == Done) { if (auto* dw = findParentComponentOfClass<DialogWindow>()) dw->exitModalState(0); return; }
        if (cfg.engine == 1) {
            ensureModelForStems();
            if (! isInstalled(cfg.model) || exeBox.state != 1) { startDownload(cfg.model, true); return; } }       
        beginSplit(); }
    void beginSplit() { job = cfg; cancelFlag = false; dlMode = false; progress = 0.f; setStatus("Starting..."); state = Running; reveal.setVisible(false); setBusy(true); startThread(); resized(); repaint(); }
    void setBusy(bool b) { engine.setEnabled(! b); for (auto& r : rows) r.c->setEnabled(! b); if (! b) syncAiChips(); updateGo(); }
    void finished(int result, const String& message, std::vector<StemOut> stems) {         
        if (result == 0) { state = Idle; setStatus({}); }
        else if (result == 1) { state = Done; setStatus("Added " + String((int) stems.size()) + (stems.size() == 1 ? " stem" : " stems") + " as new tracks"); reveal.setVisible(savedDir.isDirectory());
            if (job.engine == 1 && ! isInstalled(job.model)) { cfg.installed |= 1 << job.model; modelPick.installed = true; modelPick.repaint(); changed(); }      
            if (onDone) onDone(stems); }
        else { state = Failed; setStatus("Could not split this audio"); AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Split into Stems", message); }
        setBusy(false); resized(); repaint(); }
    void run() override {                                                                      
        std::vector<StemOut> out; String err; File tmp;
        if (job.engine == 0) runQuick(out, err); else runDemucs(out, err, tmp);
        if (cancelFlag.load() || threadShouldExit()) { if (tmp != File()) tmp.deleteRecursively(); report(0, {}, {}); return; }
        if (err.isEmpty() && job.saveFiles) { setStatus("Saving stem files..."); saveStems(out); }
        if (tmp != File()) tmp.deleteRecursively();
        report(err.isEmpty() ? 1 : 2, err, std::move(out)); }
    void report(int result, const String& msg, std::vector<StemOut> stems) {
        auto s = std::make_shared<std::vector<StemOut>>(std::move(stems)); Component::SafePointer<StemSplitterPanel> sp(this);
        MessageManager::callAsync([sp, result, msg, s] { if (sp != nullptr) sp->finished(result, msg, std::move(*s)); }); }
    void runQuick(std::vector<StemOut>& out, String& err) {
        stem::Settings ss; ss.fftSize = job.quality == 0 ? 1024 : job.quality == 2 ? 4096 : 2048; ss.sharpness = job.sharpness; ss.harmSmoothMs = job.sustainMs; ss.percWidthHz = job.drumWidthHz; ss.bassHz = job.bassHz;
        ss.zones = true; ss.guitarTopHz = job.guitarTopHz; ss.pianoTopHz = job.pianoTopHz; ss.vocalTopHz = job.vocalTopHz;
        auto on = [&](const char* k) { return ((job.qMask >> stemui::stemIndex(k)) & 1) != 0; };
        ss.guitar = on("guitar"); ss.piano = on("piano"); ss.bass = on("bass"); ss.vocals = on("vocals"); ss.drums = on("drums"); ss.other = on("other");
        auto r = stem::split(src->data(), (int) src->size(), srcSr, ss, [this](float f) { progress = f; setStatus(f < 0.15f ? "Analysing audio..." : f < 0.70f ? "Separating hits from sustained sound..." : "Rendering stems..."); }, &cancelFlag);
        std::vector<float> rest(src->begin(), src->end());                                          
        auto add = [&](const char* key, std::vector<float>& v) { if (v.empty()) return; const size_t m = jmin(v.size(), rest.size()); for (size_t i = 0; i < m; ++i) rest[i] -= v[i];
            StemOut o; o.name = stemui::nameFor(key); o.colour = stemui::colourFor(key); o.wav = std::make_shared<std::vector<float>>(std::move(v)); o.sr = srcSr; out.push_back(std::move(o)); };
        add("guitar", r.guitar); add("piano", r.piano); add("bass", r.bass); add("vocals", r.vocals); add("drums", r.drums); add("other", r.other);
        if (! out.empty() && (job.qMask & 63) != 63) { StemOut o; o.name = stemui::nameFor("rest"); o.colour = stemui::colourFor("rest"); o.wav = std::make_shared<std::vector<float>>(std::move(rest)); o.sr = srcSr; out.push_back(std::move(o)); }
        if (out.empty() && ! cancelFlag.load()) err = "Nothing was produced. Try a longer clip or a different setting."; }
    void runDemucs(std::vector<StemOut>& out, String& err, File& tmp) {
        if (! exeFile.existsAsFile()) { err = "Demucs was not found. Install it with \"pip install demucs\"."; return; }
        tmp = File::getSpecialLocation(File::tempDirectory).getChildFile("MiniDAW-stems-" + String::toHexString(Random::getSystemRandom().nextInt64()));
        if (! tmp.createDirectory().wasOk()) { err = "Could not create a temporary folder."; return; }
        const File in = tmp.getChildFile("input.wav"), outDir = tmp.getChildFile("out"); setStatus("Preparing audio...");
        if (! stemui::writeWav(in, src->data(), (int) src->size(), srcSr)) { err = "Could not write the temporary audio file."; return; }
        StringArray args { exeFile.getFullPathName(), "-n", stemui::kModelInfo[job.model].key, "--shifts", String(job.shifts), "--overlap", String(job.overlap, 2), "-o", outDir.getFullPathName(), "--filename", "{stem}.{ext}", "--float32" };
        if (job.mode > 0) args.add(String("--two-stems=") + stemui::kTwoStems[job.mode]);
        if (job.device == 1) { args.add("-d"); args.add("cpu"); } else if (job.device == 2) { args.add("-d"); args.add("mps"); } else if (job.device == 3) { args.add("-d"); args.add("cuda"); }
        args.add(in.getFullPathName());
        const int passes = jmax(1, job.shifts) * ((job.model == 1 || job.model == 3 || job.model == 5) ? 4 : 1);      
        setStatus("Loading the AI model..."); progress = 0.02f;
        ChildProcess cp; { const ScopedLock l(procLock); if (! cp.start(args)) { err = "Could not start Demucs at " + exeFile.getFullPathName(); return; } active = &cp; }
        String tail; char buf[2048]; int pass = 0, lastPct = -1;
        for (;;) {
            const int n = cp.readProcessOutput(buf, (int) sizeof(buf) - 1); if (n <= 0) break;
            const String chunk = String::fromUTF8(buf, n); tail = (tail + chunk).getLastCharacters(1600);
            const int pc = chunk.lastIndexOfChar('%');
            if (pc > 0) { int s = pc; while (s > 0 && CharacterFunctions::isDigit(chunk[s - 1])) --s;
                if (s < pc) { const int pct = jlimit(0, 100, chunk.substring(s, pc).getIntValue()); if (pct + 20 < lastPct) pass = jmin(passes - 1, pass + 1); lastPct = pct;
                    progress = 0.04f + 0.92f * jmin(1.f, ((float) pass + (float) pct / 100.f) / (float) passes); setStatus(passes > 1 ? "Separating (pass " + String(pass + 1) + " of " + String(passes) + ")..." : String("Separating...")); } }
            if (cancelFlag.load()) break; }
        cp.waitForProcessToFinish(3000); { const ScopedLock l(procLock); active = nullptr; }
        if (cancelFlag.load()) return;
        if (cp.getExitCode() != 0) { err = "Demucs stopped with an error:\n\n" + tail.trim().getLastCharacters(900); return; }
        setStatus("Importing stems..."); progress = 0.97f; std::vector<std::pair<String, File>> files;
        for (auto& f : outDir.findChildFiles(File::findFiles, true, "*.wav")) files.emplace_back(f.getFileNameWithoutExtension().toLowerCase(), f);
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return stemui::orderOf(a.first) < stemui::orderOf(b.first); });
        std::vector<std::pair<String, StemOut>> all;
        for (auto& kv : files) { double sr = 44100; auto v = stemui::readMono(kv.second, sr); if (! v) continue; StemOut o; o.name = stemui::nameFor(kv.first); o.colour = stemui::colourFor(kv.first); o.wav = v; o.sr = sr; o.file = kv.second; all.emplace_back(kv.first, std::move(o)); }
        std::vector<StemOut> restParts;                                                            
        for (auto& kv : all) { const int si = stemui::stemIndex(kv.first); if (si >= 0 && ((job.aMask >> si) & 1) != 0) out.push_back(std::move(kv.second)); else restParts.push_back(std::move(kv.second)); }
        if (! restParts.empty()) { size_t len = restParts[0].wav->size(); for (auto& r : restParts) len = jmin(len, r.wav->size());
            if (len > 0) { auto mix = std::make_shared<std::vector<float>>(len, 0.f); for (auto& r : restParts) for (size_t i = 0; i < len; ++i) (*mix)[i] += (*r.wav)[i];
                StemOut o; o.name = stemui::nameFor("rest"); o.colour = stemui::colourFor("rest"); o.wav = mix; o.sr = restParts[0].sr; out.push_back(std::move(o)); } }
        if (out.empty()) err = "Demucs finished but no stem files were found.\n\n" + tail.trim().getLastCharacters(600); }
    void saveStems(const std::vector<StemOut>& stems) {
        savedDir = File::getSpecialLocation(File::userMusicDirectory).getChildFile("MiniDAW Stems").getChildFile(File::createLegalFileName(name));
        if (! savedDir.createDirectory().wasOk()) { savedDir = File(); return; }
        for (auto& s : stems) { const File dst = savedDir.getChildFile(File::createLegalFileName(s.name) + ".wav"); if (s.file.existsAsFile()) { dst.deleteFile(); s.file.copyFileTo(dst); } else stemui::writeWav(dst, s.wav->data(), (int) s.wav->size(), s.sr); } }
    String name; std::shared_ptr<std::vector<float>> src; double srcSr = 44100; std::vector<float> peaks;
    State state = Idle; StemSettings job; std::atomic<float> progress { 0.f }; std::atomic<bool> cancelFlag { false };
    CriticalSection statusLock, procLock; String status; ChildProcess* active = nullptr; File exeFile, savedDir; std::unique_ptr<FileChooser> chooser;
    std::shared_ptr<stemui::DlShared> dl; bool dlMode = false, dlThenSplit = false; int dlModel = 0; Component::SafePointer<CallOutBox> picker;          
    StemSegmented engine; StemChipGrid quickChips, aiChips; ComboBox quality, device; Slider sharp, sustain, drumW, bassHz, gTop, pTop, vTop, shifts, overlap; StemExeBox exeBox; StemModelPicker modelPick;
    ToggleButton toggleMute, toggleSave; TextButton go { "Split into Stems" }, reveal { "Show Files" };
    StemBody body; Viewport vp; OwnedArray<StemHelpButton> helpBtns;
    JUCE_DECLARE_NON_COPYABLE(StemSplitterPanel)
};
