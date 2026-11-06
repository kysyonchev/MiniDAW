#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>
#include "AmpSim.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Look & feel: dark panel, amber accents, knobs with a 0-10 dash scale
// ═══════════════════════════════════════════════════════════════════════════
class AmpSimLnF : public juce::LookAndFeel_V4 {
public:
    static juce::Colour amber()   { return juce::Colour(0xffe8952e); }
    static juce::Colour textHi()  { return juce::Colour(0xfff1f1f5); }
    static juce::Colour textLo()  { return juce::Colour(0xffa4a5ae); }
    static juce::Colour bgDeep()  { return juce::Colour(0xff141518); }
    static juce::Colour card()    { return juce::Colour(0xff212227); }
    static juce::Colour cardEdge(){ return juce::Colour(0xff363740); }

    AmpSimLnF() {
        using juce::Colour;
        setColour(juce::ComboBox::backgroundColourId,  Colour(0xff2c2d34));
        setColour(juce::ComboBox::textColourId,        textHi());
        setColour(juce::ComboBox::outlineColourId,     Colour(0xff4b4c56));
        setColour(juce::ComboBox::arrowColourId,       amber());
        setColour(juce::PopupMenu::backgroundColourId, Colour(0xff25262c));
        setColour(juce::PopupMenu::textColourId,       textHi());
        setColour(juce::PopupMenu::highlightedBackgroundColourId, amber());
        setColour(juce::PopupMenu::highlightedTextColourId,       Colour(0xff111113));
        setColour(juce::TextButton::buttonColourId,    Colour(0xff373841));
        setColour(juce::TextButton::buttonOnColourId,  amber());
        setColour(juce::TextButton::textColourOffId,   Colour(0xffe0e0e6));
        setColour(juce::TextButton::textColourOnId,    Colour(0xff111113));
        setColour(juce::Slider::trackColourId,         amber());
        setColour(juce::Slider::backgroundColourId,    Colour(0xff111215));
        setColour(juce::Slider::thumbColourId,         textHi());
        setColour(juce::Slider::textBoxTextColourId,   textHi());
        setColour(juce::Slider::textBoxBackgroundColourId, Colour(0xff17181b));
        setColour(juce::Slider::textBoxOutlineColourId,    Colour(0xff3a3b44));
        setColour(juce::BubbleComponent::backgroundColourId, Colour(0xff0c0c0e));
        setColour(juce::BubbleComponent::outlineColourId,    amber());
        setColour(juce::TooltipWindow::backgroundColourId,   Colour(0xff0c0c0e));
        setColour(juce::TooltipWindow::textColourId,         textHi());
        setColour(juce::TooltipWindow::outlineColourId,      amber());
        setColour(juce::ScrollBar::thumbColourId,      Colour(0xff565762));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(juce::FontOptions(13.f)); }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::Font(juce::FontOptions(12.5f, juce::Font::bold)); }

    // rounded, softly shaded buttons; toggled-on buttons get an amber rim
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                              bool over, bool down) override {
        auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        auto c = bg;
        if (down) c = c.darker(0.15f); else if (over) c = c.brighter(0.08f);
        g.setGradientFill(juce::ColourGradient(c.brighter(0.07f), r.getX(), r.getY(),
                                               c.darker(0.12f),   r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle(r, 6.f);
        g.setColour(b.getToggleState() ? amber().brighter(0.2f) : juce::Colour(0xff4b4c56));
        g.drawRoundedRectangle(r, 6.f, 1.f);
    }

    void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override {
        auto r = juce::Rectangle<float>(0.f, 0.f, (float) w, (float) h).reduced(0.5f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff34353d), 0.f, 0.f, juce::Colour(0xff26272d), 0.f, (float) h, false));
        g.fillRoundedRectangle(r, 6.f);
        g.setColour(box.hasKeyboardFocus(true) ? amber() : juce::Colour(0xff4b4c56));
        g.drawRoundedRectangle(r, 6.f, 1.f);
        juce::Path p; const float cx = (float) w - 14.f, cy = (float) h * 0.5f;
        p.addTriangle(cx - 4.f, cy - 2.f, cx + 4.f, cy - 2.f, cx, cy + 3.f);
        g.setColour(amber());
        g.fillPath(p);
    }

    // 0-10 scale: 11 dashes + numbers around the outside of the knob. Dashes up to the current
    // value light up amber (with a soft glow). Works for any slider range - it just follows the knob position.
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                          float a0, float a1, juce::Slider& s) override {
        using juce::Colour;
        const auto b = juce::Rectangle<int>(x, y, w, h).toFloat();
        const float R = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;
        const auto c = b.getCentre();
        const float fh = juce::jlimit(8.f, 12.5f, R * 0.21f);
        const bool allNumbers = R >= 30.f;
        const float tickLen = juce::jlimit(3.f, 7.f, R * 0.11f);
        const float knobR = juce::jmax(8.f, R - (fh + 2.f) - tickLen - 5.f);
        const float tickIn = knobR + 3.5f, tickOut = tickIn + tickLen, numR = tickOut + 3.f + fh * 0.5f;
        auto at = [&](float ang, float r) { return juce::Point<float>(c.x + r * std::sin(ang), c.y - r * std::cos(ang)); };

        g.setFont(juce::Font(juce::FontOptions(fh, juce::Font::bold)));
        for (int i = 0; i <= 10; ++i) {
            const float t = (float) i / 10.f, ang = a0 + t * (a1 - a0);
            const bool lit = t <= pos + 0.001f, major = (i % 5) == 0;
            if (lit) { g.setColour(amber().withAlpha(0.22f)); g.drawLine({ at(ang, tickIn), at(ang, tickOut) }, major ? 5.f : 4.f); }
            g.setColour(lit ? amber() : Colour(0xff5d5e68));
            g.drawLine({ at(ang, tickIn), at(ang, tickOut) }, major ? 2.4f : 1.7f);
            if (allNumbers || major) {
                g.setColour(lit ? Colour(0xfff7e3c6) : textLo());
                g.drawText(juce::String(i), juce::Rectangle<float>(fh * 2.2f, fh + 3.f).withCentre(at(ang, numR)),
                           juce::Justification::centred, false);
            }
        }

        const bool hot = s.isMouseOverOrDragging();
        g.setColour(juce::Colours::black.withAlpha(0.5f));                                  // drop shadow
        g.fillEllipse(juce::Rectangle<float>(knobR * 2.f + 3.f, knobR * 2.f + 3.f).withCentre({ c.x, c.y + 2.f }));
        juce::ColourGradient body(Colour(0xff6f707a), c.x - knobR * 0.45f, c.y - knobR * 0.55f,
                                  Colour(0xff17181c), c.x + knobR * 0.7f, c.y + knobR * 0.8f, true);
        g.setGradientFill(body);
        g.fillEllipse(juce::Rectangle<float>(knobR * 2.f, knobR * 2.f).withCentre(c));
        g.setColour(juce::Colours::white.withAlpha(0.07f));                                  // knurled edge
        for (int i = 0; i < 28; ++i) {
            const float a = (float) i * juce::MathConstants<float>::twoPi / 28.f;
            g.drawLine({ at(a, knobR * 0.86f), at(a, knobR * 0.98f) }, 1.f);
        }
        g.setColour(hot ? amber().withAlpha(0.9f) : Colour(0xff0b0b0d));                     // rim
        g.drawEllipse(juce::Rectangle<float>(knobR * 2.f, knobR * 2.f).withCentre(c), hot ? 1.8f : 1.5f);
        const float capR = knobR * 0.64f;                                                    // inner cap
        juce::ColourGradient cap(Colour(0xff45464e), c.x, c.y - capR, Colour(0xff222328), c.x, c.y + capR, false);
        g.setGradientFill(cap);
        g.fillEllipse(juce::Rectangle<float>(capR * 2.f, capR * 2.f).withCentre(c));
        g.setColour(juce::Colours::white.withAlpha(0.09f));                                  // soft highlight
        g.fillEllipse(juce::Rectangle<float>(capR * 1.1f, capR * 0.7f).withCentre({ c.x - capR * 0.25f, c.y - capR * 0.45f }));

        const float ang = a0 + pos * (a1 - a0);                                              // pointer
        juce::Path p; p.startNewSubPath(at(ang, knobR * 0.2f)); p.lineTo(at(ang, knobR * 0.92f));
        g.setColour(amber());
        g.strokePath(p, juce::PathStrokeType(juce::jlimit(2.f, 3.4f, knobR * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Shared knob setup + the amp's knob table
// ═══════════════════════════════════════════════════════════════════════════
namespace ampui {
inline void styleKnob(juce::Slider& k, double lo, double hi, double def, int decimals, const juce::String& suffix) {
    k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);            // number box removed - the 0-10 scale replaces it
    k.setRotaryParameters(1.25f * juce::MathConstants<float>::pi, 2.75f * juce::MathConstants<float>::pi, true);
    k.setRange(lo, hi, 0.001);
    k.setNumDecimalPlacesToDisplay(decimals);
    k.setTextValueSuffix(suffix);
    k.setDoubleClickReturnValue(true, def);
    k.setPopupDisplayEnabled(true, true, nullptr, 350);                 // exact value pops up while hovering / dragging
}

struct KnobDef { const char* key; const char* label; float lo, hi, def; int dec; const char* suffix; const char* tip; };
struct GroupDef { const char* title; int count; };

// Order matters: knobs are laid out left to right, group by group.
inline const std::vector<KnobDef>& knobDefs() {
    static const std::vector<KnobDef> v = {
        { "input_gain", "INPUT",    0.f,   3.f,   1.f,   2, "",     "Input level into the pedals and amp" },
        { "drive",      "DRIVE",    0.f,   1.f,   .35f,  2, "",     "Preamp gain - how hard the tube stages are pushed" },
        { "tight",      "TIGHT",    0.f,   1.f,   .30f,  2, "",     "Cuts low end before the gain stages - tighter palm mutes, less flub" },
        { "asym",       "ASYM",     0.f,   1.f,   0.f,   2, "",     "Asymmetric clipping - adds even harmonics (warmer, fuzzier)" },
        { "bass_db",    "BASS",    -12.f,  12.f,  0.f,   1, " dB",  "Low shelf (tone stack, ahead of the last gain stage)" },
        { "mid_db",     "MID",     -12.f,  12.f,  0.f,   1, " dB",  "Midrange" },
        { "treble_db",  "TREBLE",  -12.f,  12.f,  0.f,   1, " dB",  "High shelf" },
        { "presence_db","PRESENCE", 0.f,   8.f,   0.f,   1, " dB",  "Power-amp presence - opens up the top end" },
        { "sag",        "SAG",      0.f,   1.f,   .35f,  2, "",     "Power-amp push and supply sag - squash, bloom and feel" },
        { "master",     "MASTER",   0.f,   1.5f,  .6f,   2, "",     "Output level" },
        { "cab_res_db", "CAB RES",  0.f,   8.f,   0.f,   1, " dB",  "Speaker / cabinet low resonance" },
        { "cab_lp_hz",  "CAB LP",   2000.f,8000.f,5500.f,0, " Hz",  "Speaker roll-off (4th-order low-pass)" },
        { "mic",        "MIC",      0.f,   1.f,   .30f,  2, "",     "Mic position: on-axis and bright (0) to off-axis and dark (10)" },
        { "reverb",     "REVERB",   0.f,   1.f,   0.f,   2, "",     "Built-in room reverb" },
    };
    return v;
}
inline const std::vector<GroupDef>& knobGroups() {
    static const std::vector<GroupDef> v = { { "PREAMP", 4 }, { "TONE", 3 }, { "POWER AMP", 3 }, { "CABINET", 3 }, { "ROOM", 1 } };
    return v;
}
} // namespace ampui

// ═══════════════════════════════════════════════════════════════════════════
//  Round footswitch with an LED
// ═══════════════════════════════════════════════════════════════════════════
class StompSwitch : public juce::Button {
public:
    StompSwitch() : juce::Button(juce::String()) {
        setClickingTogglesState(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto b = getLocalBounds().toFloat().reduced(1.f);
        const bool on = getToggleState();
        g.setGradientFill(juce::ColourGradient(juce::Colour(down ? 0xff2a2b31 : 0xff3a3b44), b.getX(), b.getY(),
                                               juce::Colour(0xff25262b), b.getX(), b.getBottom(), false));
        g.fillRoundedRectangle(b, 6.f);
        g.setColour(over ? juce::Colour(0xff6a6b76) : juce::Colour(0xff4b4c56));
        g.drawRoundedRectangle(b, 6.f, 1.f);
        const juce::Point<float> led(b.getX() + 15.f, b.getCentreY());
        if (on) { g.setColour(juce::Colour(0xffff4b3a).withAlpha(0.28f)); g.fillEllipse(juce::Rectangle<float>(22.f, 22.f).withCentre(led)); }
        g.setColour(on ? juce::Colour(0xffff4b3a) : juce::Colour(0xff4a2a2a));
        g.fillEllipse(juce::Rectangle<float>(9.f, 9.f).withCentre(led));
        g.setColour(juce::Colours::white.withAlpha(on ? 0.55f : 0.15f));
        g.fillEllipse(juce::Rectangle<float>(3.f, 3.f).withCentre(led.translated(-1.5f, -1.5f)));
        g.setColour(on ? AmpSimLnF::textHi() : AmpSimLnF::textLo());
        g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
        g.drawText(on ? "ON" : "OFF", b.withTrimmedLeft(28.f), juce::Justification::centred);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  AmpSimPedalTile — one tile in the pedalboard
// ═══════════════════════════════════════════════════════════════════════════
class AmpSimPedalTile : public juce::Component {
public:
    static int widthFor(const ampsim::PedalKind kind) {
        const auto* m = ampsim::metaOf(kind); const int n = m ? (int) m->knobs.size() : 0;
        return 34 + 88 * (n <= 2 ? juce::jmax(2, n) : 3);
    }
    static constexpr int kHeight = 276;

    // colour coding by pedal family
    static juce::Colour accentFor(ampsim::PedalKind k) {
        using K = ampsim::PedalKind;
        switch (k) {
            case K::DS1: case K::Rat:                       return juce::Colour(0xffff8a3d);   // distortion
            case K::OD1: case K::SD1: case K::TS9: case K::Klon: return juce::Colour(0xff32d74b);   // overdrive
            case K::Muff: case K::FuzzFace:                 return juce::Colour(0xffbf5af2);   // fuzz
            case K::Comp: case K::EQ5:                      return juce::Colour(0xff64d2ff);   // dynamics / EQ
            case K::Chorus: case K::Flanger: case K::Phaser: case K::Tremolo: return juce::Colour(0xff40e0c8);   // modulation
            case K::Delay: case K::Tape: case K::Reverb:    return juce::Colour(0xff7d7aff);   // time
            case K::BD2: case K::OCD:                       return juce::Colour(0xff32d74b);   // overdrive
            case K::HM2:                                    return juce::Colour(0xffff8a3d);   // distortion
            case K::ToneBender: case K::Octavia:            return juce::Colour(0xffbf5af2);   // fuzz
            case K::Boost: case K::NoiseGate:               return juce::Colour(0xff64d2ff);   // boost / dynamics
            case K::Wah: case K::EnvFilter: case K::Octaver:return juce::Colour(0xffffd60a);   // filter / pitch
            case K::Univibe: case K::HarmTrem:              return juce::Colour(0xff40e0c8);   // modulation
        }
        return AmpSimLnF::amber();
    }

    AmpSimPedalTile(ampsim::StompPedal& p, juce::CriticalSection& lock, int slot,
                    std::function<void(int)> onRemove,
                    std::function<void(int, int)> onMove)
        : pedal_(p), lock_(lock), slot_(slot),
          onRemove_(std::move(onRemove)), onMove_(std::move(onMove)) {
        toggle_.setToggleState(pedal_.enabled, juce::dontSendNotification);
        toggle_.setTooltip("Pedal on / off");
        toggle_.onClick = [this] {
            { const juce::ScopedLock sl(lock_); pedal_.enabled = toggle_.getToggleState(); }
            repaint();
        };
        addAndMakeVisible(toggle_);

        postB_.setClickingTogglesState(true);
        postB_.setToggleState(pedal_.post, juce::dontSendNotification);
        postB_.setButtonText(pedal_.post ? "POST" : "PRE");
        postB_.setTooltip("PRE = in front of the amp (stomp-box style)   POST = after the amp and cab (effects-loop style)");
        postB_.onClick = [this] {
            { const juce::ScopedLock sl(lock_); pedal_.post = postB_.getToggleState(); }
            postB_.setButtonText(postB_.getToggleState() ? "POST" : "PRE");
        };
        addAndMakeVisible(postB_);

        leftBtn_.setButtonText("<");
        rightBtn_.setButtonText(">");
        removeBtn_.setButtonText("X");
        leftBtn_.setTooltip("Move left");  rightBtn_.setTooltip("Move right"); removeBtn_.setTooltip("Remove pedal");
        leftBtn_.onClick   = [this] { if (onMove_)   onMove_(slot_, -1); };
        rightBtn_.onClick  = [this] { if (onMove_)   onMove_(slot_, +1); };
        removeBtn_.onClick = [this] { if (onRemove_) onRemove_(slot_);     };
        addAndMakeVisible(leftBtn_);
        addAndMakeVisible(rightBtn_);
        addAndMakeVisible(removeBtn_);

        if (const auto* meta = ampsim::metaOf(pedal_.kind)) {
            for (const auto& kd : meta->knobs) {
                auto* k = new juce::Slider;
                const int dp = kd.hi > 100.f ? 0 : 2;
                ampui::styleKnob(*k, kd.lo, kd.hi, kd.def, dp, kd.hi > 100.f ? " ms" : "");
                auto it = pedal_.params.find(kd.name);
                k->setValue(it != pedal_.params.end() ? it->second : kd.def, juce::dontSendNotification);
                const std::string kname = kd.name;
                k->onValueChange = [this, k, kname] { const juce::ScopedLock sl(lock_); pedal_.setParam(kname, k->getValue()); };
                knobs_.add(k);
                addAndMakeVisible(k);
                auto* l = new juce::Label({}, kd.label);
                l->setJustificationType(juce::Justification::centred);
                l->setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
                l->setColour(juce::Label::textColourId, AmpSimLnF::textHi());
                l->setInterceptsMouseClicks(false, false);
                labels_.add(l);
                addAndMakeVisible(l);
            }
        }
    }

    void paint(juce::Graphics& g) override {
        auto rf = getLocalBounds().toFloat().reduced(1.5f);
        const bool on = pedal_.enabled;
        const auto accent = accentFor(pedal_.kind);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(rf.translated(0.f, 2.f), 10.f);
        juce::ColourGradient bg(juce::Colour(on ? 0xff30313a : 0xff27282e), rf.getX(), rf.getY(),
                                juce::Colour(on ? 0xff25262c : 0xff1f2024), rf.getX(), rf.getBottom(), false);
        g.setGradientFill(bg);
        g.fillRoundedRectangle(rf, 10.f);
        if (on) { g.setColour(accent.withAlpha(0.16f)); g.drawRoundedRectangle(rf.expanded(1.5f), 11.f, 3.f); }
        g.setColour(on ? accent : juce::Colour(0xff40414a));
        g.drawRoundedRectangle(rf, 10.f, 1.6f);
        g.setColour(accent.withAlpha(on ? 0.9f : 0.3f));                                    // family stripe
        g.fillRoundedRectangle(rf.getX() + 16.f, rf.getY() + 2.f, rf.getWidth() - 32.f, 3.f, 1.5f);

        const auto* meta = ampsim::metaOf(pedal_.kind);
        auto top = getLocalBounds().reduced(12, 0).removeFromTop(38).withTrimmedTop(4);
        g.setColour(on ? AmpSimLnF::textHi() : AmpSimLnF::textLo());
        g.setFont(juce::Font(juce::FontOptions(15.f, juce::Font::bold)));
        g.drawText(meta ? juce::String(meta->display) : juce::String("?"), top.removeFromLeft(top.getWidth() * 3 / 5),
                   juce::Justification::centredLeft, true);
        g.setColour(AmpSimLnF::textLo());
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawText(meta ? juce::String(meta->brand).toUpperCase() : juce::String(), top, juce::Justification::centredRight, true);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(10, 6);
        r.removeFromTop(34);                                       // title row (painted)
        auto bar = r.removeFromTop(26);
        toggle_.setBounds(bar.removeFromLeft(56));
        bar.removeFromLeft(4);
        postB_.setBounds(bar.removeFromLeft(44));
        removeBtn_.setBounds(bar.removeFromRight(24));
        bar.removeFromRight(3);
        rightBtn_.setBounds(bar.removeFromRight(24));
        bar.removeFromRight(3);
        leftBtn_.setBounds(bar.removeFromRight(24));
        r.removeFromTop(6);

        const int n = knobs_.size();
        if (n == 0) return;
        const int cols = n <= 2 ? n : 3, rows = (n + cols - 1) / cols;
        const int cw = r.getWidth() / cols, cellH = juce::jmin(104, r.getHeight() / rows);
        const int y0 = r.getY() + (r.getHeight() - cellH * rows) / 2;
        for (int i = 0; i < n; ++i) {
            const int col = i % cols, row = i / cols;
            auto cell = juce::Rectangle<int>(r.getX() + col * cw, y0 + row * cellH, cw, cellH);
            labels_[i]->setBounds(cell.removeFromBottom(16));
            knobs_[i]->setBounds(cell);
        }
    }

private:
    ampsim::StompPedal& pedal_;
    juce::CriticalSection& lock_;
    int slot_;
    std::function<void(int)> onRemove_;
    std::function<void(int, int)> onMove_;
    StompSwitch toggle_;
    juce::TextButton leftBtn_ { "" }, rightBtn_ { "" }, removeBtn_ { "" }, postB_ { "PRE" };
    juce::OwnedArray<juce::Slider> knobs_;
    juce::OwnedArray<juce::Label>  labels_;
};

// ═══════════════════════════════════════════════════════════════════════════
//  AmpSimPanel — full editor
// ═══════════════════════════════════════════════════════════════════════════
class AmpSimPanel : public juce::Component, private juce::Timer {
public:
    // `lock` must be the same CriticalSection the audio callback holds while it processes
    // (Project::lock in main.cpp). Every edit takes it, so the audio thread never sees
    // half-updated filter coefficients or a pedalboard that is being modified.
    AmpSimPanel(ampsim::AmpSimProcessor& proc, juce::CriticalSection& lock, const juce::String& title)
        : proc_(proc), lock_(lock), title_(title) {
        setLookAndFeel(&lnf_);
        setOpaque(true);

        titleLbl_.setText(title_, juce::dontSendNotification);
        titleLbl_.setFont(juce::Font(juce::FontOptions(20.f, juce::Font::bold)));
        titleLbl_.setColour(juce::Label::textColourId, AmpSimLnF::textHi());
        addAndMakeVisible(titleLbl_);

        powerB_.setClickingTogglesState(true);
        powerB_.setToggleState(proc_.enabled, juce::dontSendNotification);
        powerB_.setButtonText(proc_.enabled ? "ON" : "OFF");
        powerB_.setTooltip("Amp on / off");
        powerB_.onClick = [this] {
            { const juce::ScopedLock sl(lock_); proc_.enabled = powerB_.getToggleState(); }
            powerB_.setButtonText(powerB_.getToggleState() ? "ON" : "OFF");
        };
        addAndMakeVisible(powerB_);

        presetB_.addItem("(custom)", 1);
        for (size_t i = 0; i < ampsim::presets().size(); ++i)
            presetB_.addItem(ampsim::presets()[i].name, (int) i + 2);
        presetB_.setSelectedId(1, juce::dontSendNotification);
        presetB_.onChange = [this] { loadPreset(presetB_.getSelectedId()); };
        addAndMakeVisible(presetB_);

        prevB_.setButtonText("<"); nextB_.setButtonText(">");
        prevB_.setTooltip("Previous preset"); nextB_.setTooltip("Next preset");
        prevB_.onClick = [this] { stepPreset(-1); };
        nextB_.onClick = [this] { stepPreset(+1); };
        addAndMakeVisible(prevB_);
        addAndMakeVisible(nextB_);

        for (size_t i = 0; i < ampsim::voicings().size(); ++i)
            voiceB_.addItem(ampsim::voicings()[i].name, (int) i + 1);
        voiceB_.setSelectedId(proc_.params.voice + 1, juce::dontSendNotification);
        voiceB_.setTooltip("Amp voicing: how the gain stages, tone stack and power amp behave");
        voiceB_.onChange = [this] {
            { const juce::ScopedLock sl(lock_); proc_.params.voice = voiceB_.getSelectedId() - 1; }
            presetB_.setSelectedId(1, juce::dontSendNotification);
        };
        addAndMakeVisible(voiceB_);

        for (const auto& d : ampui::knobDefs()) {
            auto* s = new juce::Slider;
            ampui::styleKnob(*s, d.lo, d.hi, d.def, d.dec, d.suffix);
            s->setValue(getParam(d.key), juce::dontSendNotification);
            s->setTooltip(d.tip);
            const char* key = d.key;
            s->onValueChange = [this, s, key] {
                setParam(key, (float) s->getValue());
                presetB_.setSelectedId(1, juce::dontSendNotification);
                if (!std::strcmp(key, "cab_res_db") || !std::strcmp(key, "cab_lp_hz") || !std::strcmp(key, "presence_db") || !std::strcmp(key, "mic"))
                    cabB_.setSelectedId(1, juce::dontSendNotification);
            };
            ampKnobs_.add(s);
            addAndMakeVisible(s);

            auto* l = new juce::Label({}, d.label);
            l->setJustificationType(juce::Justification::centred);
            l->setColour(juce::Label::textColourId, AmpSimLnF::textHi());
            l->setFont(juce::Font(juce::FontOptions(12.f, juce::Font::bold)));
            l->setInterceptsMouseClicks(false, false);
            knobLabels_.add(l);
            addAndMakeVisible(l);
        }

        cabB_.addItem("(custom cab)", 1);
        for (size_t i = 0; i < ampsim::cabSims().size(); ++i)
            cabB_.addItem(ampsim::cabSims()[i].name, (int) i + 2);
        cabB_.setSelectedId(1, juce::dontSendNotification);
        cabB_.onChange = [this] { loadCab(cabB_.getSelectedId()); };
        addAndMakeVisible(cabB_);

        gateB_.setClickingTogglesState(true);
        gateB_.setToggleState(proc_.params.noise_gate_enabled, juce::dontSendNotification);
        gateB_.setButtonText(proc_.params.noise_gate_enabled ? "GATE ON" : "GATE OFF");
        gateB_.setTooltip("Noise gate in front of the pedals");
        gateB_.onClick = [this] {
            { const juce::ScopedLock sl(lock_); proc_.params.noise_gate_enabled = gateB_.getToggleState(); }
            gateB_.setButtonText(gateB_.getToggleState() ? "GATE ON" : "GATE OFF");
        };
        addAndMakeVisible(gateB_);

        gateThr_.setSliderStyle(juce::Slider::LinearHorizontal);
        gateThr_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        gateThr_.setRange(-80.0, -10.0, 0.5);
        gateThr_.setTextValueSuffix(" dB");
        gateThr_.setDoubleClickReturnValue(true, -50.0);
        gateThr_.setValue(proc_.params.noise_gate_thresh_db, juce::dontSendNotification);
        gateThr_.onValueChange = [this] {
            const juce::ScopedLock sl(lock_); proc_.params.noise_gate_thresh_db = (float) gateThr_.getValue(); };
        addAndMakeVisible(gateThr_);

        {
            const auto& metas = ampsim::pedalMetas();
            int firstId = 0;
            for (const char* cat : ampsim::pedalCategories()) {
                addPedalB_.addSectionHeading(cat);
                for (size_t i = 0; i < metas.size(); ++i)
                    if (!std::strcmp(ampsim::categoryOf(metas[i].kind), cat)) {
                        addPedalB_.addItem(juce::String(metas[i].brand) + " " + juce::String(metas[i].display), (int) i + 1);
                        if (firstId == 0) firstId = (int) i + 1;
                    }
            }
            if (firstId > 0) addPedalB_.setSelectedId(firstId, juce::dontSendNotification);
        }
        addAndMakeVisible(addPedalB_);

        boardB_.addItem("Load a rig...", 1);
        for (size_t i = 0; i < ampsim::boardPresets().size(); ++i)
            boardB_.addItem(ampsim::boardPresets()[i].name, (int) i + 2);
        boardB_.setSelectedId(1, juce::dontSendNotification);
        boardB_.setTooltip("Replace the pedalboard with a ready-made rig");
        boardB_.onChange = [this] { loadBoard(boardB_.getSelectedId()); };
        addAndMakeVisible(boardB_);

        addPedalBtn_.onClick = [this] { addPedal(); };
        addAndMakeVisible(addPedalBtn_);

        pedalView_.setViewedComponent(&pedalStrip_, false);
        pedalView_.setScrollBarsShown(false, true);
        pedalView_.setScrollBarThickness(9);
        addAndMakeVisible(pedalView_);

        rebuildPedalTiles();
        setSize(1280, 760);     // last: resized() touches every child above, so they must exist first
        startTimerHz(30);
    }

    ~AmpSimPanel() override { stopTimer(); setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1d1e23), 0.f, 0.f, AmpSimLnF::bgDeep(), 0.f, (float) getHeight(), false));
        g.fillAll();

        for (auto cr : { ampCardR_, pedalCardR_ }) {
            auto f = cr.toFloat();
            g.setColour(juce::Colours::black.withAlpha(0.35f)); g.fillRoundedRectangle(f.translated(0.f, 2.f), 12.f);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff25262c), 0.f, f.getY(), AmpSimLnF::card(), 0.f, f.getBottom(), false));
            g.fillRoundedRectangle(f, 12.f);
            g.setColour(AmpSimLnF::cardEdge()); g.drawRoundedRectangle(f.reduced(0.5f), 12.f, 1.f);
        }

        // header tag
        g.setColour(AmpSimLnF::amber());
        g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        g.drawText("AMP SIM", titleR_.withHeight(16), juce::Justification::centredLeft);
        g.fillRoundedRectangle((float) titleR_.getX(), (float) titleR_.getBottom() + 1.f, 34.f, 2.f, 1.f);

        g.setFont(juce::Font(juce::FontOptions(12.f, juce::Font::bold)));
        g.setColour(AmpSimLnF::amber());
        g.drawText("AMPLIFIER",  ampTitleR_,   juce::Justification::centredLeft);
        g.drawText("PEDALBOARD", pedalTitleR_, juce::Justification::centredLeft);
        g.setColour(AmpSimLnF::textLo());
        g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
        g.drawText("THRESHOLD", gateLabelR_, juce::Justification::centredRight);

        // knob groups: soft panel, small caption, hairline under it
        const auto& groups = ampui::knobGroups();
        for (int i = 0; i < (int) groups.size() && i < groupR_.size(); ++i) {
            auto f = groupR_[i].toFloat();
            g.setColour(juce::Colour(0x0fffffff)); g.fillRoundedRectangle(f, 9.f);
            g.setColour(juce::Colour(0x1affffff)); g.drawRoundedRectangle(f.reduced(0.5f), 9.f, 1.f);
            g.setColour(AmpSimLnF::textLo());
            g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
            g.drawText(groups[(size_t) i].title, groupTitleR_[i], juce::Justification::centredLeft);
            g.setColour(juce::Colour(0x22ffffff));
            g.drawHorizontalLine(groupTitleR_[i].getBottom() + 1, f.getX() + 8.f, f.getRight() - 8.f);
        }

        drawLevel(g, inBar_,  dispIn_,  -1.f,   "IN",  false);
        drawLevel(g, outBar_, dispOut_, holdOut_, "OUT", clipT_ > 0);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(18, 12);
        auto top = r.removeFromTop(52);
        titleR_ = top.removeFromLeft(220);
        titleLbl_.setBounds(titleR_.withTrimmedTop(16));
        powerB_.setBounds(top.removeFromLeft(70).reduced(0, 10));
        top.removeFromLeft(14);
        outBar_ = top.removeFromRight(230).withSizeKeepingCentre(230, 24);
        top.removeFromRight(10);
        inBar_  = top.removeFromRight(230).withSizeKeepingCentre(230, 24);
        top.removeFromRight(16);
        auto pr = top.removeFromLeft(juce::jmin(400, top.getWidth())).reduced(0, 10);
        prevB_.setBounds(pr.removeFromLeft(32));  pr.removeFromLeft(4);
        nextB_.setBounds(pr.removeFromRight(32)); pr.removeFromRight(4);
        presetB_.setBounds(pr);

        r.removeFromTop(10);
        ampCardR_ = r.removeFromTop(274);
        {
            auto c = ampCardR_.reduced(16, 12);
            auto hdr = c.removeFromTop(30);
            ampTitleR_ = hdr.removeFromLeft(110);
            voiceB_.setBounds(hdr.removeFromLeft(190));
            hdr.removeFromLeft(10);
            cabB_.setBounds(hdr.removeFromLeft(210));
            hdr.removeFromLeft(18);
            gateB_.setBounds(hdr.removeFromLeft(96));
            hdr.removeFromLeft(8);
            gateLabelR_ = hdr.removeFromLeft(90);
            hdr.removeFromLeft(8);
            gateThr_.setBounds(hdr.removeFromLeft(juce::jmin(300, hdr.getWidth())));
            c.removeFromTop(8);

            const auto& groups = ampui::knobGroups();
            const int nG = (int) groups.size(), nK = ampKnobs_.size(), gap = 10, pad = 6;
            const int kw = (c.getWidth() - nG * 2 * pad - (nG - 1) * gap) / juce::jmax(1, nK);
            groupR_.clear(); groupTitleR_.clear();
            int ki = 0, x = c.getX();
            for (int g = 0; g < nG; ++g) {
                const int cnt = groups[(size_t) g].count;
                auto gr = juce::Rectangle<int>(x, c.getY(), cnt * kw + 2 * pad, c.getHeight());
                groupR_.add(gr);
                auto inner = gr.reduced(pad, 6);
                auto tr = inner.removeFromTop(16);
                groupTitleR_.add(tr.withTrimmedLeft(2));
                for (int k = 0; k < cnt && ki < nK; ++k, ++ki) {
                    auto cell = inner.removeFromLeft(kw);
                    knobLabels_[ki]->setBounds(cell.removeFromBottom(20));
                    ampKnobs_[ki]->setBounds(cell);
                }
                x += gr.getWidth() + gap;
            }
        }

        r.removeFromTop(12);
        pedalCardR_ = r;
        {
            auto c = pedalCardR_.reduced(16, 12);
            auto hdr = c.removeFromTop(32);
            pedalTitleR_ = hdr.removeFromLeft(130);
            addPedalB_.setBounds(hdr.removeFromLeft(280));
            hdr.removeFromLeft(10);
            addPedalBtn_.setBounds(hdr.removeFromLeft(110));
            hdr.removeFromLeft(24);
            boardB_.setBounds(hdr.removeFromLeft(240));
            c.removeFromTop(8);
            pedalView_.setBounds(c);
        }
        layoutPedalStrip();
    }

private:
    float getParam(const char* k) const {
        const auto& p = proc_.params;
        if (!std::strcmp(k, "input_gain"))  return p.input_gain;
        if (!std::strcmp(k, "drive"))       return p.drive;
        if (!std::strcmp(k, "tight"))       return p.tight;
        if (!std::strcmp(k, "asym"))        return p.asym;
        if (!std::strcmp(k, "bass_db"))     return p.bass_db;
        if (!std::strcmp(k, "mid_db"))      return p.mid_db;
        if (!std::strcmp(k, "treble_db"))   return p.treble_db;
        if (!std::strcmp(k, "presence_db")) return p.presence_db;
        if (!std::strcmp(k, "sag"))         return p.sag;
        if (!std::strcmp(k, "master"))      return p.master;
        if (!std::strcmp(k, "cab_res_db"))  return p.cab_res_db;
        if (!std::strcmp(k, "cab_lp_hz"))   return p.cab_lp_hz;
        if (!std::strcmp(k, "mic"))         return p.mic;
        if (!std::strcmp(k, "reverb"))      return p.reverb;
        return 0.f;
    }

    // The processor notices parameter changes by itself at the start of the next block,
    // so there is nothing to recompute here - just write the value under the audio lock.
    void setParam(const char* k, float v) {
        const juce::ScopedLock sl(lock_);
        auto& p = proc_.params;
        if      (!std::strcmp(k, "input_gain"))  p.input_gain  = v;
        else if (!std::strcmp(k, "drive"))       p.drive       = v;
        else if (!std::strcmp(k, "tight"))       p.tight       = v;
        else if (!std::strcmp(k, "asym"))        p.asym        = v;
        else if (!std::strcmp(k, "bass_db"))     p.bass_db     = v;
        else if (!std::strcmp(k, "mid_db"))      p.mid_db      = v;
        else if (!std::strcmp(k, "treble_db"))   p.treble_db   = v;
        else if (!std::strcmp(k, "presence_db")) p.presence_db = v;
        else if (!std::strcmp(k, "sag"))         p.sag         = v;
        else if (!std::strcmp(k, "master"))      p.master      = v;
        else if (!std::strcmp(k, "cab_res_db"))  p.cab_res_db  = v;
        else if (!std::strcmp(k, "cab_lp_hz"))   p.cab_lp_hz   = v;
        else if (!std::strcmp(k, "mic"))         p.mic         = v;
        else if (!std::strcmp(k, "reverb"))      p.reverb      = v;
    }

    void syncKnobsFromParams() {
        const auto& defs = ampui::knobDefs();
        for (int i = 0; i < ampKnobs_.size() && i < (int) defs.size(); ++i)
            ampKnobs_[i]->setValue(getParam(defs[(size_t) i].key), juce::dontSendNotification);
        voiceB_.setSelectedId(proc_.params.voice + 1, juce::dontSendNotification);
        repaint();
    }

    void stepPreset(int dir) {
        const int n = (int) ampsim::presets().size();
        int id = presetB_.getSelectedId();
        if (id < 2) id = dir > 0 ? 2 : n + 1;
        else        id = 2 + ((id - 2 + dir) % n + n) % n;
        presetB_.setSelectedId(id, juce::sendNotificationSync);
    }

    void loadPreset(int id) {
        if (id < 2 || id - 2 >= (int) ampsim::presets().size()) return;
        const auto& preset = ampsim::presets()[(size_t)(id - 2)];
        {
            const juce::ScopedLock sl(lock_);
            const bool gateOn = proc_.params.noise_gate_enabled;       // presets don't own the gate
            const float gateThr = proc_.params.noise_gate_thresh_db;
            proc_.params = preset.p;
            proc_.params.noise_gate_enabled = gateOn;
            proc_.params.noise_gate_thresh_db = gateThr;
            proc_.recompute();
            proc_.reset();
        }
        cabB_.setSelectedId(1, juce::dontSendNotification);
        syncKnobsFromParams();
    }

    void loadCab(int id) {
        if (id < 2 || id - 2 >= (int) ampsim::cabSims().size()) return;
        const auto& c = ampsim::cabSims()[(size_t)(id - 2)];
        {
            const juce::ScopedLock sl(lock_);
            proc_.params.cab_res_db = c.res_db;
            proc_.params.cab_lp_hz = c.lp_hz;
            proc_.params.presence_db = c.presence_db;
            proc_.params.mic = c.mic;
        }
        syncKnobsFromParams();
    }

    void addPedal() {
        const int idx = addPedalB_.getSelectedId() - 1;
        if (idx < 0 || idx >= (int)ampsim::pedalMetas().size()) return;
        bool added = false;
        { const juce::ScopedLock sl(lock_); added = proc_.pedalboard.add(ampsim::pedalMetas()[(size_t)idx].kind) != nullptr; }
        if (!added) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Pedalboard",
                "The pedalboard is full (" + juce::String(ampsim::PedalBoard::kMaxPedals) + " pedals max).");
            return;
        }
        rebuildPedalTiles();
        scrollToEnd();
    }

    void loadBoard(int id) {
        if (id < 2 || id - 2 >= (int) ampsim::boardPresets().size()) return;
        { const juce::ScopedLock sl(lock_); ampsim::loadBoardPreset(proc_.pedalboard, ampsim::boardPresets()[(size_t)(id - 2)]); }
        boardB_.setSelectedId(1, juce::dontSendNotification);
        rebuildPedalTiles();
    }

    void doRemovePedal(int slot) {
        { const juce::ScopedLock sl(lock_); proc_.pedalboard.removeAt(slot); }
        rebuildPedalTiles();
    }
    void doMovePedal(int from, int delta) {
        { const juce::ScopedLock sl(lock_); proc_.pedalboard.move(from, from + delta); }
        rebuildPedalTiles();
    }

    void schedulePedalRemove(int slot) {
        juce::Component::SafePointer<AmpSimPanel> sp(this);
        juce::MessageManager::callAsync([sp, slot] {
            if (sp != nullptr) sp->doRemovePedal(slot);
        });
    }
    void schedulePedalMove(int from, int delta) {
        juce::Component::SafePointer<AmpSimPanel> sp(this);
        juce::MessageManager::callAsync([sp, from, delta] {
            if (sp != nullptr) sp->doMovePedal(from, delta);
        });
    }

    void rebuildPedalTiles() {
        pedalStrip_.removeAllChildren();   // detach first
        pedalTiles_.clear();               // then delete
        for (int i = 0; i < (int)proc_.pedalboard.pedals.size(); ++i) {
            auto* tile = new AmpSimPedalTile(
                *proc_.pedalboard.pedals[(size_t)i], lock_, i,
                [this](int s)        { schedulePedalRemove(s); },
                [this](int f, int d) { schedulePedalMove(f, d); });
            pedalTiles_.add(tile);
            pedalStrip_.addAndMakeVisible(tile);
        }
        layoutPedalStrip();
    }

    void scrollToEnd() { pedalView_.setViewPosition(juce::jmax(0, pedalStrip_.getWidth() - pedalView_.getWidth()), 0); }

    void layoutPedalStrip() {
        const int gap = 10, pad = 2, tileH = AmpSimPedalTile::kHeight;
        int x = pad;
        for (int i = 0; i < pedalTiles_.size(); ++i) {
            const int w = AmpSimPedalTile::widthFor(proc_.pedalboard.pedals[(size_t) i]->kind);
            pedalTiles_[i]->setBounds(x, pad, w, tileH);
            x += w + gap;
        }
        pedalStrip_.setSize(juce::jmax(x + pad, pedalView_.getMaximumVisibleWidth()), tileH + pad * 2);
    }

    // segmented LED-style meter with optional peak-hold marker and clip lamp
    void drawLevel(juce::Graphics& g, juce::Rectangle<int> r, float v, float hold, const juce::String& label, bool clipLamp) {
        auto rf = r.toFloat();
        g.setColour(juce::Colour(0xff0f1013));
        g.fillRoundedRectangle(rf, 5.f);
        g.setColour(juce::Colour(0xff3d3e47));
        g.drawRoundedRectangle(rf.reduced(0.5f), 5.f, 1.f);

        auto bar = rf.reduced(4.f, 6.f).withTrimmedLeft(26.f).withTrimmedRight(clipLamp || hold >= 0.f ? 14.f : 2.f);
        constexpr int segs = 28; constexpr float gap = 1.5f;
        const float sw = (bar.getWidth() - gap * (segs - 1)) / (float) segs;
        auto pos = [](float lv) { return juce::jlimit(0.f, 1.f, (20.f * std::log10(juce::jmax(1.0e-6f, lv)) + 60.f) / 66.f); };
        const float t = pos(v), th = hold >= 0.f ? pos(hold) : -1.f;
        const int holdSeg = th >= 0.f ? juce::jmin(segs - 1, (int) (th * (float) segs)) : -1;
        for (int i = 0; i < segs; ++i) {
            const float p0 = (float) i / (float) segs;
            const juce::Colour c = p0 < 0.62f ? juce::Colour(0xff32d74b) : p0 < 0.86f ? juce::Colour(0xffffd60a) : juce::Colour(0xffff453a);
            g.setColour((p0 < t || (i == holdSeg && th > 0.f)) ? c : c.withAlpha(0.13f));
            g.fillRoundedRectangle(bar.getX() + (float) i * (sw + gap), bar.getY(), sw, bar.getHeight(), 1.f);
        }
        if (clipLamp || hold >= 0.f) {                                                       // clip lamp
            const auto lamp = juce::Rectangle<float>(8.f, 8.f).withCentre({ rf.getRight() - 10.f, rf.getCentreY() });
            if (clipLamp) { g.setColour(juce::Colour(0xffff453a).withAlpha(0.35f)); g.fillEllipse(lamp.expanded(3.f)); }
            g.setColour(clipLamp ? juce::Colour(0xffff453a) : juce::Colour(0xff3a2224));
            g.fillEllipse(lamp);
        }
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.setFont(juce::Font(juce::FontOptions(11.f, juce::Font::bold)));
        g.drawText(label, r.withTrimmedLeft(9).withWidth(26), juce::Justification::centredLeft);
    }

    void timerCallback() override {
        const float i = proc_.in_level, o = proc_.out_level;       // smoothed so the meters fall gently
        dispIn_  = juce::jmax(i, dispIn_  * 0.82f);
        dispOut_ = juce::jmax(o, dispOut_ * 0.82f);

        const float pk = proc_.out_peak;                            // peak hold: sticks ~0.9 s, then falls
        if (pk >= holdOut_) { holdOut_ = pk; holdT_ = 27; }
        else if (holdT_ > 0) --holdT_;
        else holdOut_ *= 0.90f;

        if (proc_.clip) { proc_.clip = false; clipT_ = 45; }        // latch is cleared here, lamp lingers 1.5 s
        else if (clipT_ > 0) --clipT_;

        repaint(inBar_.getUnion(outBar_));
    }

    AmpSimLnF lnf_;                       // declared first => destroyed last
    juce::TooltipWindow tooltip_ { nullptr, 450 };
    ampsim::AmpSimProcessor& proc_;
    juce::CriticalSection& lock_;
    juce::String title_;
    juce::Label titleLbl_;
    juce::TextButton powerB_, prevB_, nextB_;
    juce::ComboBox presetB_, cabB_, voiceB_;
    juce::OwnedArray<juce::Slider> ampKnobs_;
    juce::OwnedArray<juce::Label>  knobLabels_;
    juce::TextButton gateB_ { "GATE OFF" };
    juce::Slider gateThr_;
    juce::ComboBox addPedalB_, boardB_;
    juce::TextButton addPedalBtn_ { "+ ADD PEDAL" };
    juce::Viewport pedalView_;
    juce::Component pedalStrip_;
    juce::OwnedArray<AmpSimPedalTile> pedalTiles_;
    juce::Rectangle<int> inBar_, outBar_, ampCardR_, pedalCardR_, ampTitleR_, pedalTitleR_, gateLabelR_, titleR_;
    juce::Array<juce::Rectangle<int>> groupR_, groupTitleR_;
    float dispIn_ = 0.f, dispOut_ = 0.f, holdOut_ = 0.f;
    int holdT_ = 0, clipT_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Serialization helpers (XmlElement round-trip)
// ═══════════════════════════════════════════════════════════════════════════
namespace ampsim_io {

inline void toXml(const ampsim::AmpSimProcessor& p, juce::XmlElement& e) {
    e.setAttribute("enabled",     p.enabled ? 1 : 0);
    e.setAttribute("input_gain",  (double)p.params.input_gain);
    e.setAttribute("drive",       (double)p.params.drive);
    e.setAttribute("asym",        (double)p.params.asym);
    e.setAttribute("bass_db",     (double)p.params.bass_db);
    e.setAttribute("mid_db",      (double)p.params.mid_db);
    e.setAttribute("treble_db",   (double)p.params.treble_db);
    e.setAttribute("cab_res_db",  (double)p.params.cab_res_db);
    e.setAttribute("cab_lp_hz",   (double)p.params.cab_lp_hz);
    e.setAttribute("presence_db", (double)p.params.presence_db);
    e.setAttribute("master",      (double)p.params.master);
    e.setAttribute("gate_on",     p.params.noise_gate_enabled ? 1 : 0);
    e.setAttribute("gate_thr",    (double)p.params.noise_gate_thresh_db);
    e.setAttribute("voice",       p.params.voice);
    e.setAttribute("tight",       (double)p.params.tight);
    e.setAttribute("sag",         (double)p.params.sag);
    e.setAttribute("mic",         (double)p.params.mic);
    e.setAttribute("reverb",      (double)p.params.reverb);

    for (const auto& ped : p.pedalboard.pedals) {
        auto* pe = e.createNewChildElement("PEDAL");
        const auto* meta = ampsim::metaOf(ped->kind);
        pe->setAttribute("key", meta ? juce::String(meta->key) : juce::String());
        pe->setAttribute("on",  ped->enabled ? 1 : 0);
        pe->setAttribute("post", ped->post ? 1 : 0);
        for (const auto& kv : ped->params) {
            const juce::String attr = juce::String("p_") + juce::String(kv.first.c_str());
            pe->setAttribute(attr, kv.second);
        }
    }
}

inline void fromXml(ampsim::AmpSimProcessor& p, const juce::XmlElement& e) {
    p.enabled                    = e.getIntAttribute("enabled", 1) != 0;
    p.params.input_gain          = (float)e.getDoubleAttribute("input_gain", 1.0);
    p.params.drive               = (float)e.getDoubleAttribute("drive", 0.35);
    p.params.asym                = (float)e.getDoubleAttribute("asym", 0.0);
    p.params.bass_db             = (float)e.getDoubleAttribute("bass_db", 0.0);
    p.params.mid_db              = (float)e.getDoubleAttribute("mid_db", 0.0);
    p.params.treble_db           = (float)e.getDoubleAttribute("treble_db", 0.0);
    p.params.cab_res_db          = (float)e.getDoubleAttribute("cab_res_db", 0.0);
    p.params.cab_lp_hz           = (float)e.getDoubleAttribute("cab_lp_hz", 5500.0);
    p.params.presence_db         = (float)e.getDoubleAttribute("presence_db", 0.0);
    p.params.master              = (float)e.getDoubleAttribute("master", 0.6);
    p.params.noise_gate_enabled  = e.getIntAttribute("gate_on", 0) != 0;
    p.params.noise_gate_thresh_db= (float)e.getDoubleAttribute("gate_thr", -50.0);
    // new in the 4x engine - projects saved before it simply get the defaults
    p.params.voice               = juce::jlimit(0, (int) ampsim::voicings().size() - 1, e.getIntAttribute("voice", 1));
    p.params.tight               = (float)e.getDoubleAttribute("tight", 0.30);
    p.params.sag                 = (float)e.getDoubleAttribute("sag", 0.35);
    p.params.mic                 = (float)e.getDoubleAttribute("mic", 0.30);
    p.params.reverb              = (float)e.getDoubleAttribute("reverb", 0.0);

    p.pedalboard.clear();
    for (auto* pe : e.getChildWithTagNameIterator("PEDAL")) {
        const auto* meta = ampsim::findMeta(pe->getStringAttribute("key").toStdString());
        if (!meta) continue;
        auto* ped = p.pedalboard.add(meta->kind);
        if (!ped) break;                      // board full
        ped->enabled = pe->getIntAttribute("on", 1) != 0;
        ped->post    = pe->getIntAttribute("post", 0) != 0;      // older projects: everything stays in front of the amp
        std::vector<std::pair<std::string, double>> updates;
        for (auto& kv : ped->params) {
            const juce::String attr = juce::String("p_") + juce::String(kv.first.c_str());
            if (pe->hasAttribute(attr))
                updates.emplace_back(kv.first, pe->getDoubleAttribute(attr, kv.second));
        }
        for (auto& up : updates) ped->setParam(up.first, up.second);
    }
    p.recompute();
    p.reset();
}

} // namespace ampsim_io
