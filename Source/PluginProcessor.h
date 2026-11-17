#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Diagnostics.h"
#include "Parameters.h"
#include "dsp/DisplayModel.h"
#include "dsp/Engine.h"
#include "dsp/RefEq.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

// Single-producer / single-consumer float ring (audio thread → UI).
class SpscRing
{
public:
    explicit SpscRing (int capacityPow2 = 1 << 16) : buf ((size_t) capacityPow2), mask (capacityPow2 - 1) {}
    void push (const float* x, int n) noexcept
    {
        int w = write.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) buf[(size_t) ((w + i) & mask)] = x[i];
        write.store (w + n, std::memory_order_release);
    }
    // copies everything written since the last call (at most capacity)
    int pull (std::vector<float>& out) noexcept
    {
        const int w = write.load (std::memory_order_acquire);
        int n = w - readPos;
        if (n > mask + 1) { readPos = w - (mask + 1); n = mask + 1; }
        out.resize ((size_t) n);
        for (int i = 0; i < n; ++i) out[(size_t) i] = buf[(size_t) ((readPos + i) & mask)];
        readPos = w;
        return n;
    }
private:
    std::vector<float> buf;
    int mask;
    std::atomic<int> write { 0 };
    int readPos = 0;
};

class VT4AudioProcessor : public juce::AudioProcessor
{
    // declared first: destroyed last, so the log sees the whole teardown
    struct Teardown
    {
        vt4diag::Instance diag;
        double started = 0.0;
        ~Teardown() { if (started > 0.0) vt4diag::log ("[#" + juce::String (diag.id()) + "] destroyed in " + juce::String (vt4diag::nowMs() - started, 1) + " ms"); }
    } teardown;

public:
    vt4diag::Instance& diag() noexcept { return teardown.diag; }

    VT4AudioProcessor();
    ~VT4AudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void setNonRealtime (bool isNonRealtime) noexcept override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    // ---- shared with the editor ----------------------------------------------
    std::shared_ptr<vt4p::UiFlags> flags = std::make_shared<vt4p::UiFlags>();
    juce::AudioProcessorValueTreeState apvts;
    vt4p::ParamPointers pp;

    vt4::DeviceParams currentParams() const { return pp.read(); }

    // meters
    std::array<std::atomic<float>, 2> vu { { 0.0f, 0.0f } };          // VU needle (1.0 = 0 VU)
    std::array<std::atomic<float>, 2> peak { { 0.0f, 0.0f } };        // block peak (UI resets)
    std::atomic<float> bplus { 300.0f }, bplusNominal { 300.0f };
    std::atomic<float> stress { 0.0f };                               // UI resets
    std::atomic<float> warmth { 1.0f };                               // warm-up progress

    // analyser feeds (mono sum, host rate)
    SpscRing anaIn, anaOut;

    // audition (written by the editor)
    std::atomic<int> auditionMode { 0 }, auditionSet { 0 }, auditionBand { 0 };
    std::atomic<int> soloType { 0 };
    std::atomic<float> soloF1 { 100.0f }, soloF2 { 1000.0f };

    // display support (valid after prepareToPlay)
    std::atomic<int> preparedCount { 0 };
    double getHostRate() const noexcept { return hostRate; }
    double getOversampledRate() const noexcept { return osRate; }
    std::vector<double> oversamplingResponse (const vt4::DisplayModel& grid) const;
    static std::vector<double> oversamplingResponseFor (double hostRate, int osOrder, const vt4::DisplayModel& grid);
    int getOversamplingOrder() const noexcept { return osOrder; }
    bool circuitRunning() const noexcept { return engineLive; }
    int activeModel() const noexcept { return targetModel; }

private:
    void processChunk (juce::AudioBuffer<float>&, int start, int num, int numIn);

    juce::AudioParameterBool* bypassParam = nullptr;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    vt4::DeltaEngine engine;
    vt4::SoloEngine solo;
    // MPEQ / PSEQ: linear reference models (the circuit is out of the path)
    vt4::RefEq refs[2];
    double modelW[3] { 1.0, 0.0, 0.0 };   // cross-fade weights VT-4 / MPEQ / PSEQ
    double modelStep = 1.0e-4;
    int targetModel = 0;
    bool engineRest = false;             // a reference model is on: the circuit is not computed
    int restIdle = 0;
    vt4::DeviceParams lastParams;
    int osOrder = 2;
    double hostRate = 44100.0, osRate = 176400.0;

    juce::AudioBuffer<float> dryBuffer, work;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 4096 };
    juce::SmoothedValue<float> wetMix { 1.0f }, outGain { 1.0f };
    int maxBlock = 512;
    bool prepared = false;
    bool engineLive = false;     // the circuit is built and running
    bool sleeping = false;       // bypassed long enough: the circuit is not computed
    int bypassIdle = 0;
    bool lastBypassed = false, lastSwapping = false;

    // VU ballistics (IEC 60268-17: 99 % in 300 ms, 1.3 % overshoot)
    struct VuState { double y = 0.0, v = 0.0; } vuState[2];
    double vuW = 13.5, vuZ = 0.81;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VT4AudioProcessor)
};
