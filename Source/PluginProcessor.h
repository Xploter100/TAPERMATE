#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

class TapermateAudioProcessor : public juce::AudioProcessor
{
public:
    TapermateAudioProcessor();
    ~TapermateAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TAPERMATE"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // ---- datos para la interfaz (medidores / carretes) ----
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int bufSize  = 1 << 16;

    std::atomic<float> peakDb { -100.f };
    std::atomic<float> reelRate { 0.f };
    std::atomic<int>   blockCounter { 0 };
    std::array<std::atomic<float>, fftSize / 2> spectrum;

private:
    using Coeffs = juce::dsp::IIR::Coefficients<float>;
    using Filter = juce::dsp::IIR::Filter<float>;

    float readDelay (const std::vector<float>& b, float delay) const;

    double sr = 44100.0;

    // linea de retardo (cabezal de cinta / pitch)
    std::array<std::vector<float>, 2> delayBuf;
    int   writePos = 0;
    float phase = 0.f, wowPhase = 0.f, flutPhase = 0.f;
    float ratioSmooth = 1.f, mixB = 0.f;

    // stop de cinta
    bool  wasStop = false;
    float stopT = 0.f, stopStartRatio = 1.f, stopGain = 1.f;

    // ruido
    juce::Random rng;
    float prevNoise[2] { 0.f, 0.f };

    // EQ / color
    Filter hp[2], lp[2], mid[2], bump[2];
    float lastLow = -1.f, lastHigh = -1.f, lastMidDb = 999.f, lastMidFreq = -1.f;

    juce::Reverb reverb;

    // analizador
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, fftSize>     fifo {};
    std::array<float, fftSize * 2> fftData {};
    int fifoIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapermateAudioProcessor)
};
