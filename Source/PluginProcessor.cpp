#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace { constexpr int kMask = TapermateAudioProcessor::bufSize - 1; }

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout TapermateAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    auto addF = [&] (const char* id, const char* name, float lo, float hi, float step, float skew, float def)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                      NormalisableRange<float> (lo, hi, step, skew), def));
    };

    addF ("noise",   "Noise",    0.f,   100.f,  0.1f, 1.f,  15.f);
    addF ("reverb",  "Reverb",   0.f,   100.f,  0.1f, 1.f,  20.f);
    addF ("drive",   "Drive",    0.f,   100.f,  0.1f, 1.f,  30.f);
    addF ("lowcut",  "Low Cut",  20.f,  500.f,  1.f,  0.5f, 20.f);
    addF ("midcut",  "Mid Cut",  -24.f, 0.f,    0.1f, 1.f,  0.f);
    addF ("midfreq", "Mid Freq", 200.f, 5000.f, 1.f,  0.5f, 1000.f);
    addF ("highcut", "High Cut", 1000.f, 20000.f, 1.f, 0.35f, 20000.f);

    l.add (std::make_unique<AudioParameterInt> (ParameterID { "pitch", 1 }, "Pitch", -12, 0, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 }, "Speed",
                                                   StringArray { "1x", "1.5x", "2x", "STOP" }, 0));
    return l;
}

TapermateAudioProcessor::TapermateAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& s : spectrum) s.store (-100.f);
}

bool TapermateAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto o = layouts.getMainOutputChannelSet();
    if (o != juce::AudioChannelSet::mono() && o != juce::AudioChannelSet::stereo()) return false;
    return o == layouts.getMainInputChannelSet();
}

void TapermateAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    for (auto& b : delayBuf) b.assign ((size_t) bufSize, 0.f);
    writePos = 0; phase = 0.f; wowPhase = 0.f; flutPhase = 0.f;
    ratioSmooth = 1.f; mixB = 0.f; wasStop = false; stopGain = 1.f;
    lastLow = lastHigh = lastMidFreq = -1.f; lastMidDb = 999.f;
    fifoIndex = 0;

    for (int ch = 0; ch < 2; ++ch)
    {
        hp[ch].reset(); lp[ch].reset(); mid[ch].reset(); bump[ch].reset();
        // "head bump" de cinta: realce suave de graves
        *bump[ch].coefficients = *Coeffs::makeLowShelf (sr, 90.f, 0.7f, 1.2f);
    }
    reverb.setSampleRate (sr);
    reverb.reset();
}

float TapermateAudioProcessor::readDelay (const std::vector<float>& b, float delay) const
{
    float rp = (float) writePos - delay;
    if (rp < 0.f) rp += (float) bufSize;
    int i0 = (int) rp;
    const float f = rp - (float) i0;
    i0 &= kMask;
    const int i1 = (i0 + 1) & kMask;
    return b[(size_t) i0] + f * (b[(size_t) i1] - b[(size_t) i0]);
}

//==============================================================================
void TapermateAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n = buffer.getNumSamples();
    if (numCh == 0 || delayBuf[0].empty()) return;

    auto P = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const float noiseAmt = P ("noise")  * 0.01f;
    const float revAmt   = P ("reverb") * 0.01f;
    const float driveAmt = P ("drive")  * 0.01f;
    const float lowCut   = P ("lowcut");
    const float highCut  = P ("highcut");
    const float midDb    = P ("midcut");
    const float midFreq  = P ("midfreq");
    const int   semis    = (int) std::round (P ("pitch"));
    const int   speedIdx = (int) std::round (P ("speed"));
    const bool  stop     = (speedIdx == 3);

    static const float speedTab[3] { 1.f, 1.5f, 2.f };
    const float target = speedTab[juce::jmin (speedIdx, 2)] * std::pow (2.f, (float) semis / 12.f);

    // ---- EQ (solo recalcula si cambia algo) ----
    const float nyq = 0.45f * (float) sr;
    if (lowCut != lastLow)
    {
        auto c = Coeffs::makeHighPass (sr, juce::jmin (lowCut, nyq));
        for (int ch = 0; ch < 2; ++ch) *hp[ch].coefficients = *c;
        lastLow = lowCut;
    }
    if (highCut != lastHigh)
    {
        auto c = Coeffs::makeLowPass (sr, juce::jmin (highCut, nyq));
        for (int ch = 0; ch < 2; ++ch) *lp[ch].coefficients = *c;
        lastHigh = highCut;
    }
    if (midDb != lastMidDb || midFreq != lastMidFreq)
    {
        auto c = Coeffs::makePeakFilter (sr, juce::jmin (midFreq, nyq), 0.8f, juce::Decibels::decibelsToGain (midDb));
        for (int ch = 0; ch < 2; ++ch) *mid[ch].coefficients = *c;
        lastMidDb = midDb; lastMidFreq = midFreq;
    }

    // ---- constantes de la seccion de saturacion / ruido ----
    const float pre      = std::pow (10.f, driveAmt * 24.f / 20.f);   // 0..+24 dB
    const float bias     = 0.12f * driveAmt;                          // asimetria -> armonicos pares
    const float biasOff  = std::tanh (bias);
    const float post     = 1.f / std::sqrt (pre);
    const float dryWet   = juce::jmin (1.f, driveAmt * 8.f);
    const float noiseGain = std::pow (noiseAmt, 1.5f) * 0.05f;

    // ---- constantes del transporte ----
    const float W      = 0.05f * (float) sr;                          // ventana del pitch shifter (50 ms)
    const float dt     = 1.f / (float) sr;
    const float coefR  = 1.f - std::exp (-1.f / (0.06f * (float) sr));
    const float coefM  = 1.f - std::exp (-1.f / (0.03f * (float) sr));
    const float twoPi  = juce::MathConstants<float>::twoPi;
    const float pi     = juce::MathConstants<float>::pi;

    float* data[2] { buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr };

    for (int i = 0; i < n; ++i)
    {
        // ----- velocidad / stop de cinta -----
        if (stop)
        {
            if (! wasStop) { wasStop = true; stopT = 0.f; stopStartRatio = ratioSmooth; }
            stopT += dt;
            if      (stopT < 0.30f) ratioSmooth = stopStartRatio * (1.f - stopT / 0.30f);   // frena
            else if (stopT < 0.45f) ratioSmooth = -1.8f * std::sin (pi * (stopT - 0.30f) / 0.15f); // retroceso corto
            else                    ratioSmooth = 0.f;
            stopGain = 1.f - juce::jlimit (0.f, 1.f, (stopT - 0.33f) / 0.17f);
        }
        else
        {
            wasStop = false;
            ratioSmooth += (target - ratioSmooth) * coefR;
            stopGain    += (1.f - stopGain) * coefM;
        }

        const bool shifting = stop || std::abs (ratioSmooth - 1.f) > 0.002f;
        mixB += ((shifting ? 1.f : 0.f) - mixB) * coefM;

        // pitch shifter de dos cabezales (cinta que se frena/acelera)
        phase += (1.f - ratioSmooth) / W;
        phase -= std::floor (phase);
        float p2 = phase + 0.5f; p2 -= std::floor (p2);
        const float d1 = phase * W + 2.f, d2 = p2 * W + 2.f;
        const float s1 = std::sin (pi * phase), s2 = std::sin (pi * p2);
        const float g1 = s1 * s1, g2 = s2 * s2;

        // wow & flutter (variacion natural de la cinta)
        wowPhase  += twoPi * 0.6f / (float) sr;  if (wowPhase  > twoPi) wowPhase  -= twoPi;
        flutPhase += twoPi * 7.3f / (float) sr;  if (flutPhase > twoPi) flutPhase -= twoPi;
        const float dA = (0.005f + 0.0015f * std::sin (wowPhase) + 0.00004f * std::sin (flutPhase)) * (float) sr;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& b = delayBuf[(size_t) ch];
            b[(size_t) writePos] = data[ch][i];

            const float a = readDelay (b, dA);
            float y = a;
            if (mixB > 0.0001f)
            {
                const float w = g1 * readDelay (b, d1) + g2 * readDelay (b, d2);
                y = a + mixB * (w - a);
            }
            y *= stopGain;

            // hiss de cinta
            const float nz = rng.nextFloat() * 2.f - 1.f;
            y += (nz - prevNoise[ch]) * 0.7f * noiseGain;
            prevNoise[ch] = nz;

            // saturacion de cinta
            const float s = (std::tanh (y * pre + bias) - biasOff) * post;
            y += dryWet * (s - y);

            // color + EQ
            y = bump[ch].processSample (y);
            y = hp[ch].processSample (y);
            y = mid[ch].processSample (y);
            y = lp[ch].processSample (y);

            data[ch][i] = y;
        }
        writePos = (writePos + 1) & kMask;
    }

    // ---- reverb ----
    juce::Reverb::Parameters rp;
    rp.roomSize = 0.75f; rp.damping = 0.5f; rp.width = 1.f; rp.freezeMode = 0.f;
    rp.wetLevel = 0.45f * revAmt;
    rp.dryLevel = 1.f - 0.35f * revAmt;
    reverb.setParameters (rp);
    if (numCh == 2) reverb.processStereo (data[0], data[1], n);
    else            reverb.processMono (data[0], n);

    // ---- medidores ----
    float peak = 0.f;
    for (int ch = 0; ch < numCh; ++ch) peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, n));
    peakDb.store (juce::Decibels::gainToDecibels (peak, -100.f));
    reelRate.store (ratioSmooth);
    blockCounter.fetch_add (1);

    for (int i = 0; i < n; ++i)
    {
        float m = data[0][i];
        if (numCh > 1) m = 0.5f * (m + data[1][i]);
        fifo[(size_t) fifoIndex++] = m;
        if (fifoIndex == fftSize)
        {
            fifoIndex = 0;
            std::fill (fftData.begin(), fftData.end(), 0.f);
            std::copy (fifo.begin(), fifo.end(), fftData.begin());
            window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (fftData.data());
            for (int k = 0; k < fftSize / 2; ++k)
                spectrum[(size_t) k].store (juce::Decibels::gainToDecibels (fftData[(size_t) k] / ((float) fftSize * 0.25f), -100.f));
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* TapermateAudioProcessor::createEditor()
{
    return new TapermateAudioProcessorEditor (*this);
}

void TapermateAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void TapermateAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TapermateAudioProcessor();
}
