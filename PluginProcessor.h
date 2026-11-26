#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
// Multisample data
//==============================================================================
struct SampleZone
{
    int rootNote = 60;
    juce::AudioBuffer<float> data;   // 1 or 2 channels, 4 padding samples after "length"
    int length = 0;
    double fileRate = 44100.0;
    int loopStart = -1, loopEnd = -1; // -1 = one-shot
};

struct Instrument
{
    juce::String name;
    std::vector<SampleZone> zones;

    const SampleZone* find (int note) const
    {
        const SampleZone* best = nullptr;
        int bestDist = 100000;
        for (auto& z : zones)
        {
            const int d = std::abs (z.rootNote - note);
            if (d < bestDist) { bestDist = d; best = &z; }
        }
        return best;
    }
};

//==============================================================================
class GenSynthAudioProcessor : public juce::AudioProcessor
{
public:
    GenSynthAudioProcessor();
    ~GenSynthAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "gen synth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ~/Documents/gen synth/samples/<instrument name>/C3.wav, F#2.wav, ...
    static juce::File getSampleFolder();
    static juce::StringArray getSourceNames();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::vector<std::unique_ptr<Instrument>> userInstruments;
    std::vector<const Instrument*> instruments;   // index = "source" choice

    juce::Synthesiser synth;
    std::atomic<float> lastNote { -1.0f };

    juce::dsp::Chorus<float> chorus;
    juce::Reverb reverb;

    juce::AudioBuffer<float> delayBuf;
    int delayWrite = 0;
    float delaySmoothed = 0.0f;
    float fbLpL = 0.0f, fbLpR = 0.0f;

    double currentSampleRate = 44100.0;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenSynthAudioProcessor)
};
