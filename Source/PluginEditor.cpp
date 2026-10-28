#include "PluginEditor.h"

static const char* kNames[9] { "NOISE", "REVERB", "DRIVE", "LOW CUT", "MID CUT", "MID FREQ", "HIGH CUT", "PITCH", "SPEED" };

TapermateAudioProcessorEditor::TapermateAudioProcessorEditor (TapermateAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (800, 440);
    bandDb.fill (-100.f);

    struct Def { const char* id; const char* suffix; };
    static const Def defs[8] {
        { "noise", " %" }, { "reverb", " %" }, { "drive", " %" }, { "lowcut", " Hz" },
        { "midcut", " dB" }, { "midfreq", " Hz" }, { "highcut", " Hz" }, { "pitch", " st" } };

    for (size_t i = 0; i < 8; ++i)
    {
        auto& s = knobs[i].slider;
        s.setLookAndFeel (&laf);
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 16);
        s.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (s);
        knobs[i].att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, defs[i].id, s);

        const juce::String suffix (defs[i].suffix);
        s.textFromValueFunction = [suffix] (double v) { return juce::String ((int) std::round (v)) + suffix; };
        s.updateText();
    }

    speedBox.setLookAndFeel (&laf);
    speedBox.addItemList (juce::StringArray { "1x", "1.5x", "2x", "STOP" }, 1);
    speedBox.setColour (juce::ComboBox::backgroundColourId, juce::Colours::black);
    speedBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    speedBox.setColour (juce::ComboBox::outlineColourId, juce::Colours::white);
    speedBox.setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
    addAndMakeVisible (speedBox);
    speedAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "speed", speedBox);

    startTimerHz (30);
}

TapermateAudioProcessorEditor::~TapermateAudioProcessorEditor()
{
    stopTimer();
    for (auto& k : knobs) k.slider.setLookAndFeel (nullptr);
    speedBox.setLookAndFeel (nullptr);
}

void TapermateAudioProcessorEditor::resized()
{
    const float slotW = (float) (getWidth() - 40) / 9.f;
    for (int i = 0; i < 9; ++i)
    {
        const int x = 20 + (int) (slotW * (float) i);
        labelRects[(size_t) i] = { x, 335, (int) slotW, 14 };
        if (i < 8) knobs[(size_t) i].slider.setBounds (x, 349, (int) slotW, 80);
        else       speedBox.setBounds (x + 8, 375, (int) slotW - 16, 26);
    }
    specArea  = { 30, 175, 690, 140 };
    meterArea = { 745, 175, 22, 140 };
}

void TapermateAudioProcessorEditor::timerCallback()
{
    // los carretes giran solo si el plugin esta procesando audio
    const int bc = proc.blockCounter.load();
    const bool running = (bc != lastBlock);
    lastBlock = bc;
    const float rate = running ? proc.reelRate.load() : 0.f;
    displayRate += (rate - displayRate) * 0.35f;
    reelAngle = std::fmod (reelAngle + displayRate * 0.13f, juce::MathConstants<float>::twoPi);

    // espectro en bandas logaritmicas 30 Hz - 18 kHz
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 44100.0;
    for (int b = 0; b < numBands; ++b)
    {
        const double f0 = 30.0 * std::pow (600.0, (double) b / numBands);
        const double f1 = 30.0 * std::pow (600.0, (double) (b + 1) / numBands);
        int i0 = juce::jmax (1, (int) (f0 * TapermateAudioProcessor::fftSize / sr));
        int i1 = juce::jmax (i0, (int) (f1 * TapermateAudioProcessor::fftSize / sr));
        i1 = juce::jmin (i1, TapermateAudioProcessor::fftSize / 2 - 1);
        float m = -100.f;
        for (int i = i0; i <= i1; ++i) m = juce::jmax (m, proc.spectrum[(size_t) i].load());
        auto& d = bandDb[(size_t) b];
        d = (m > d) ? m : juce::jmax (-100.f, d - 2.f);
    }

    const float pk = proc.peakDb.load();
    peakHoldDb = (pk > peakHoldDb) ? pk : juce::jmax (-100.f, peakHoldDb - 0.8f);
    repaint();
}

void TapermateAudioProcessorEditor::drawReel (juce::Graphics& g, juce::Point<float> c, float r, float angle)
{
    g.setColour (juce::Colours::white);
    g.fillEllipse (c.x - r, c.y - r, 2.f * r, 2.f * r);

    g.setColour (juce::Colours::black);
    for (int i = 0; i < 3; ++i)
    {
        juce::Path p;
        p.addRoundedRectangle (r * 0.30f, -r * 0.10f, r * 0.56f, r * 0.20f, r * 0.06f);
        const float a = angle + (float) i * juce::MathConstants<float>::twoPi / 3.f;
        g.fillPath (p, juce::AffineTransform::rotation (a).translated (c.x, c.y));
    }
    g.fillEllipse (c.x - r * 0.18f, c.y - r * 0.18f, r * 0.36f, r * 0.36f);
    g.setColour (juce::Colours::white);
    g.fillEllipse (c.x - r * 0.07f, c.y - r * 0.07f, r * 0.14f, r * 0.14f);
}

void TapermateAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0a0a0a));

    // titulo
    g.setColour (juce::Colours::white);
    g.setFont (24.f);
    g.drawText ("TAPERMATE", 0, 78, getWidth(), 30, juce::Justification::centred);
    g.setFont (11.f);
    g.setColour (juce::Colours::grey);
    g.drawText ("TAPE EMULATOR", 0, 106, getWidth(), 16, juce::Justification::centred);

    // carretes
    const float r = 58.f;
    const juce::Point<float> c1 (230.f, 95.f), c2 (570.f, 95.f);
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.drawLine (c1.x, c1.y + r, c2.x, c2.y + r, 1.5f);
    drawReel (g, c1, r, reelAngle);
    drawReel (g, c2, r, reelAngle);

    // ---- analizador de frecuencias ----
    g.setColour (juce::Colour (0xff222222));
    g.drawRect (specArea);
    const float bw = (float) specArea.getWidth() / (float) numBands;
    for (int b = 0; b < numBands; ++b)
    {
        const float db = bandDb[(size_t) b];
        const float h = juce::jlimit (0.f, 1.f, (db + 90.f) / 90.f) * (float) specArea.getHeight();
        g.setColour (db > kLimitDb ? juce::Colour (0xffff4040) : juce::Colours::white);
        g.fillRect ((float) specArea.getX() + (float) b * bw + 1.f,
                    (float) specArea.getBottom() - h, bw - 2.f, h);
    }
    // linea del limite
    const float yLim = (float) specArea.getBottom() - ((kLimitDb + 90.f) / 90.f) * (float) specArea.getHeight();
    g.setColour (juce::Colour (0xffff4040).withAlpha (0.6f));
    g.drawHorizontalLine ((int) yLim, (float) specArea.getX(), (float) specArea.getRight());
    g.setFont (10.f);
    g.drawText ("-3 dB", specArea.getRight() - 40, (int) yLim - 12, 38, 12, juce::Justification::right);

    // etiquetas de frecuencia
    g.setColour (juce::Colours::grey);
    const double fl[3] { 100.0, 1000.0, 10000.0 };
    const char* ft[3] { "100", "1k", "10k" };
    for (int i = 0; i < 3; ++i)
    {
        const float x = (float) specArea.getX() + (float) specArea.getWidth() * (float) (std::log (fl[i] / 30.0) / std::log (600.0));
        g.drawText (ft[i], (int) x - 15, specArea.getBottom() + 2, 30, 12, juce::Justification::centred);
    }

    // ---- medidor de nivel (pico) ----
    g.setColour (juce::Colour (0xff222222));
    g.drawRect (meterArea);
    const float lvl = juce::jlimit (0.f, 1.f, (peakHoldDb + 60.f) / 60.f) * (float) meterArea.getHeight();
    const float limLvl = ((kLimitDb + 60.f) / 60.f) * (float) meterArea.getHeight();
    const float mx = (float) meterArea.getX() + 3.f, mw = (float) meterArea.getWidth() - 6.f;
    const float mb = (float) meterArea.getBottom();
    g.setColour (juce::Colours::white);
    g.fillRect (mx, mb - juce::jmin (lvl, limLvl), mw, juce::jmin (lvl, limLvl));
    if (lvl > limLvl)
    {
        g.setColour (juce::Colour (0xffff4040));
        g.fillRect (mx, mb - lvl, mw, lvl - limLvl);
    }
    g.setColour (peakHoldDb > kLimitDb ? juce::Colour (0xffff4040) : juce::Colours::grey);
    g.setFont (11.f);
    g.drawText (peakHoldDb > kLimitDb ? "OVER" : "PEAK", meterArea.getX() - 10, meterArea.getY() - 16, 42, 14, juce::Justification::centred);
    g.setColour (juce::Colours::grey);
    g.drawText (peakHoldDb <= -99.f ? "-inf" : juce::String (peakHoldDb, 1), meterArea.getX() - 10, meterArea.getBottom() + 2, 42, 12, juce::Justification::centred);

    // etiquetas de perillas
    g.setColour (juce::Colours::white);
    g.setFont (11.f);
    for (int i = 0; i < 9; ++i)
        g.drawText (kNames[i], labelRects[(size_t) i], juce::Justification::centred);
}
