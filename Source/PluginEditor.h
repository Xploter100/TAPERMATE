#pragma once
#include "PluginProcessor.h"

// Look minimalista: arcos blancos finos sobre fondo negro
struct MinimalLAF : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override
    {
        auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (8.f);
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        const auto c = b.getCentre();
        const float ang = startAngle + pos * (endAngle - startAngle);

        juce::Path track, val, ptr;
        track.addCentredArc (c.x, c.y, r, r, 0.f, startAngle, endAngle, true);
        val.addCentredArc   (c.x, c.y, r, r, 0.f, startAngle, ang, true);
        g.setColour (juce::Colour (0xff333333));
        g.strokePath (track, juce::PathStrokeType (2.f));
        g.setColour (juce::Colours::white);
        g.strokePath (val, juce::PathStrokeType (2.f));

        ptr.startNewSubPath (0.f, -r * 0.45f);
        ptr.lineTo (0.f, -r * 0.8f);
        g.strokePath (ptr, juce::PathStrokeType (2.f), juce::AffineTransform::rotation (ang).translated (c.x, c.y));
    }
};

class TapermateAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TapermateAudioProcessorEditor (TapermateAudioProcessor&);
    ~TapermateAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawReel (juce::Graphics&, juce::Point<float> centre, float radius, float angle);

    static constexpr int   numBands = 56;
    static constexpr float kLimitDb = -3.f;   // limite aceptable de nivel

    TapermateAudioProcessor& proc;
    MinimalLAF laf;

    struct Knob
    {
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    std::array<Knob, 8> knobs;
    std::array<juce::Rectangle<int>, 9> labelRects;

    juce::ComboBox speedBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> speedAtt;

    float reelAngle = 0.f, displayRate = 0.f;
    int   lastBlock = 0;
    std::array<float, numBands> bandDb {};
    float peakHoldDb = -100.f;
    juce::Rectangle<int> specArea, meterArea;
};
