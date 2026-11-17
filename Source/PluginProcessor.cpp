#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
// worker threads of the engine and the editor: low priority, logged
const bool hooksInstalled = []
{
    vt4::gWorkerThreadStart = [] { vt4diag::lowerCurrentThreadPriority(); };
    vt4::gWorkerLog = [] (const char* s) { vt4diag::log (juce::String ("worker: ") + s); };
    return true;
}();
}

VT4AudioProcessor::VT4AudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VT4", vt4p::createLayout (flags))
{
    pp.attach (apvts);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (vt4p::ids::bypass));
    jassert (bypassParam != nullptr);
    lastParams = pp.read();
    juce::ignoreUnused (hooksInstalled);
    vt4diag::log ("[#" + juce::String (diag().id()) + "] created");
}

VT4AudioProcessor::~VT4AudioProcessor()
{
    vt4diag::log ("[#" + juce::String (diag().id()) + "] destroying");
    teardown.started = vt4diag::nowMs();
}

void VT4AudioProcessor::releaseResources()
{
    vt4diag::log ("[#" + juce::String (diag().id()) + "] releaseResources");
}

void VT4AudioProcessor::setNonRealtime (bool isNonRealtime) noexcept
{
    if (isNonRealtime != this->isNonRealtime())
        vt4diag::log ("[#" + juce::String (diag().id()) + "] " + (isNonRealtime ? "offline (render) mode" : "real-time mode"));
    AudioProcessor::setNonRealtime (isNonRealtime);
}

bool VT4AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void VT4AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const double t0 = vt4diag::nowMs();
    prepared = false;
    hostRate = sampleRate;
    maxBlock = juce::jmax (1, samplesPerBlock);

    // the circuit runs at >= 176.4 kHz: 4x for 44.1 / 48 k, 2x for 88.2 / 96 k, 1x above
    osOrder = sampleRate < 70000.0 ? 2 : (sampleRate < 140000.0 ? 1 : 0);
    osRate = sampleRate * (double) (1 << osOrder);
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) 2, (size_t) osOrder, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampler->initProcessing ((size_t) maxBlock);

    lastParams = pp.read();
    flags->stepped.store (pp.stepped->load() > 0.5f);
    // Never a long wait on the host's thread: the engine restarts from its
    // pristine copy when it can, otherwise it builds in the background and
    // the plugin passes the (latency-aligned) input until it is ready.
    // Offline rendering builds here, so a bounce is processed from sample 0.
    engine.prepare (osRate, lastParams, isNonRealtime());
    engineLive = engine.ready();
    sleeping = false; bypassIdle = 0;
    solo.prepare (sampleRate);
    refs[0].prepare (osRate, lastParams, vt4::kModelMPEQ);
    refs[1].prepare (osRate, lastParams, vt4::kModelPSEQ);
    targetModel = pp.readModel();
    for (int m = 0; m < 3; ++m) modelW[m] = m == targetModel ? 1.0 : 0.0;
    modelStep = 1.0 / (0.05 * osRate);       // 50 ms cross-fade between models
    engineRest = targetModel != 0; restIdle = 0;

    const int latency = (int) std::lround (oversampler->getLatencyInSamples());
    setLatencySamples (latency);
    dryDelay.setMaximumDelayInSamples (juce::jmax (8, latency + 8));
    dryDelay.prepare ({ sampleRate, (juce::uint32) maxBlock, 2u });
    dryDelay.setDelay ((float) latency);
    dryBuffer.setSize (2, maxBlock);
    work.setSize (2, maxBlock);

    wetMix.reset (sampleRate, 0.03);
    wetMix.setCurrentAndTargetValue (bypassParam->get() || ! engineLive ? 0.0f : 1.0f);
    outGain.reset (sampleRate, 0.03);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pp.output->load()));
    vuState[0] = vuState[1] = {};
    if (engineLive) bplusNominal.store ((float) engine.device().bplus());
    prepared = true;
    preparedCount.fetch_add (1);
    auto& st = diag().stats;
    st.sampleRate.store (sampleRate); st.blockSize.store (maxBlock); st.circuitRunning.store (engineLive); st.sleeping.store (false);
    vt4diag::log ("[#" + juce::String (diag().id()) + "] prepareToPlay " + juce::String ((int) sampleRate) + " Hz, block " + juce::String (samplesPerBlock)
                  + (isNonRealtime() ? ", offline" : "") + ": " + juce::String (vt4diag::nowMs() - t0, 2) + " ms, "
                  + (engineLive ? "circuit ready" : "circuit building in the background"));
}

std::vector<double> VT4AudioProcessor::oversamplingResponse (const vt4::DisplayModel& grid) const
{
    return oversamplingResponseFor (hostRate, osOrder, grid);
}

// Magnitude of the up + down oversampling filters at the display grid, from
// their impulse response. Safe on any thread (its own filter instance).
std::vector<double> VT4AudioProcessor::oversamplingResponseFor (double hostRate, int osOrder, const vt4::DisplayModel& grid)
{
    std::vector<double> mag ((size_t) vt4::DisplayModel::kN, 1.0);
    if (osOrder == 0) return mag;
    const int N = 16384;
    juce::dsp::Oversampling<float> os (1, (size_t) osOrder, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    os.initProcessing ((size_t) N);
    juce::AudioBuffer<float> b (1, N);
    b.clear();
    b.setSample (0, 0, 1.0f);
    juce::dsp::AudioBlock<float> blk (b);
    auto up = os.processSamplesUp (blk);
    juce::ignoreUnused (up);
    os.processSamplesDown (blk);
    const float* h = b.getReadPointer (0);
    for (int i = 0; i < vt4::DisplayModel::kN; ++i)
    {
        const double f = grid.freq (i);
        if (f >= 0.5 * hostRate) break;
        // DFT at f by a rotating phasor (re-normalised in blocks)
        const double w = 2.0 * juce::MathConstants<double>::pi * f / hostRate;
        const std::complex<double> step (std::cos (w), -std::sin (w));
        std::complex<double> acc (0.0, 0.0);
        for (int n0 = 0; n0 < N; n0 += 256)
        {
            std::complex<double> z = std::polar (1.0, -w * n0);
            const int n1 = std::min (N, n0 + 256);
            for (int n = n0; n < n1; ++n) { acc += (double) h[n] * z; z *= step; }
        }
        mag[(size_t) i] = std::abs (acc);
    }
    return mag;
}

void VT4AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto tStart = juce::Time::getHighResolutionTicks();
    struct Measure
    {
        VT4AudioProcessor& p; juce::int64 t0; int n;
        ~Measure()
        {
            if (n <= 0 || p.hostRate <= 0.0) return;
            const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            p.diag().stats.addBlock (n, secs, p.hostRate);
        }
    } measure { *this, tStart, buffer.getNumSamples() };
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    if (! prepared || numIn <= 0 || buffer.getNumChannels() <= 0)
        return;

    const vt4::DeviceParams p = pp.read();
    flags->stepped.store (pp.stepped->load() > 0.5f);
    if (! vt4p::sameParams (p, lastParams))
    {
        engine.setParams (p);
        refs[0].setParams (p); refs[1].setParams (p);
        lastParams = p;
    }

    // model: VT-4 (circuit) or a reference model (linear, circuit at rest)
    {
        const int m = pp.readModel();
        if (m != targetModel)
        {
            if (m != 0 && modelW[m] == 0.0) refs[m - 1].restart (p);
            if (m == 0 && engineRest)
            {
                if (engineLive) engine.restartAtRest();
                engineRest = false;
            }
            targetModel = m;
            restIdle = 0;
        }
        if (targetModel != 0 && modelW[targetModel] >= 1.0 && ! engineRest
            && (restIdle += buffer.getNumSamples()) > (int) (0.2 * hostRate))
            engineRest = true;
    }

    engine.poll();
    if (! engine.ready() && isNonRealtime())
        engine.waitUntilReady (10000);
    if (engine.ready() && ! engineLive)
    {
        // the background build has arrived: fade the circuit in
        engineLive = true;
        if (targetModel == 0) wetMix.setCurrentAndTargetValue (0.0f);
        bplusNominal.store ((float) engine.device().bplus());
    }

    // audition requests from the editor
    const int am = auditionMode.load (std::memory_order_relaxed);
    const int aset = juce::jlimit (0, 1, auditionSet.load (std::memory_order_relaxed));
    const int aband = juce::jlimit (0, 4, auditionBand.load (std::memory_order_relaxed));
    vt4::SoloWindow w;
    w.type = soloType.load (std::memory_order_relaxed);
    w.f1 = soloF1.load (std::memory_order_relaxed);
    w.f2 = soloF2.load (std::memory_order_relaxed);
    engine.setDelta (am == (int) vt4::Audition::kDelta, aset, aband);
    solo.setRequest (am == (int) vt4::Audition::kSolo, aset, aband, p.stereo, w);

    const bool bypassed = bypassParam->get();
    wetMix.setTargetValue (bypassed || (! engineLive && targetModel == 0) ? 0.0f : 1.0f);

    // Bypassed for a while: the circuit rests (no CPU). When it is switched
    // back in it restarts at its operating point and fades in.
    if (engineLive)
    {
        if (bypassed && ! wetMix.isSmoothing() && wetMix.getCurrentValue() == 0.0f)
        {
            if (! sleeping && (bypassIdle += buffer.getNumSamples()) > (int) (0.2 * hostRate))
                sleeping = true;
        }
        else
            bypassIdle = 0;
        if (sleeping && ! bypassed)
        {
            if (! engineRest) engine.restartAtRest();
            for (int m = 1; m < 3; ++m) if (modelW[m] > 0.0) refs[m - 1].restart (p);
            oversampler->reset();
            sleeping = false;
        }
    }
    {
        auto& st = diag().stats;
        if (bypassed != lastBypassed) { st.bypassChanges.fetch_add (1, std::memory_order_relaxed); lastBypassed = bypassed; }
        st.sleeping.store (sleeping, std::memory_order_relaxed);
        st.circuitRunning.store (engineLive, std::memory_order_relaxed);
        const bool sw = engine.swapping();
        if (sw && ! lastSwapping) st.swaps.fetch_add (1, std::memory_order_relaxed);
        lastSwapping = sw;
    }
    outGain.setTargetValue (juce::Decibels::decibelsToGain (pp.output->load()));

    const int total = buffer.getNumSamples();
    for (int pos = 0; pos < total; pos += maxBlock)
        processChunk (buffer, pos, juce::jmin (maxBlock, total - pos), juce::jmin (numIn, buffer.getNumChannels()));

    if (engineLive)
    {
        bplus.store ((float) engine.device().bplus());
        warmth.store ((float) engine.device().warmth());
        const float s = (float) engine.device().takeStress();
        float prev = stress.load();
        while (s > prev && ! stress.compare_exchange_weak (prev, s)) {}
    }
}

void VT4AudioProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int num, int numIn)
{
    // input (mono hosts: the same signal on both device channels)
    for (int ch = 0; ch < 2; ++ch)
        work.copyFrom (ch, 0, buffer, juce::jmin (ch, numIn - 1), start, num);

    // dry path for bypass, aligned with the oversampling latency; analyser input
    {
        float* a = work.getWritePointer (0);
        float* b = work.getWritePointer (1);
        float* da = dryBuffer.getWritePointer (0);
        float* db = dryBuffer.getWritePointer (1);
        float mono[1024];
        for (int i = 0; i < num; ++i)
        {
            dryDelay.pushSample (0, a[i]); da[i] = dryDelay.popSample (0);
            dryDelay.pushSample (1, b[i]); db[i] = dryDelay.popSample (1);
        }
        for (int i = 0; i < num; i += 1024)
        {
            const int n = juce::jmin (1024, num - i);
            for (int k = 0; k < n; ++k) mono[k] = 0.5f * (a[i + k] + b[i + k]);
            anaIn.push (mono, n);
        }
    }

    if (! sleeping)
    {
        juce::dsp::AudioBlock<float> block (work);
        auto sub = block.getSubBlock (0, (size_t) num);
        auto up = oversampler->processSamplesUp (sub);
        const int upN = (int) up.getNumSamples();
        float* uL = up.getChannelPointer (0);
        float* uR = up.getChannelPointer (1);
        const bool runEngine = engineLive && ! engineRest;
        for (int i = 0; i < upN; ++i)
        {
            const double inL = (double) uL[i], inR = (double) uR[i];
            double l = 0.0, r = 0.0, sum = 0.0;
            for (int m = 0; m < 3; ++m)
            {
                // weights glide (50 ms) toward the selected model
                double& w = modelW[m];
                const double t = m == targetModel ? 1.0 : 0.0;
                if (w != t) w = t > w ? std::min (t, w + modelStep) : std::max (t, w - modelStep);
                if (w == 0.0) continue;
                double a = inL, b = inR;
                if (m == 0) { if (runEngine) engine.process (inL, inR, a, b); }
                else refs[m - 1].process (inL, inR, a, b);
                l += w * a; r += w * b; sum += w;
            }
            if (sum != 1.0 && sum > 0.0) { l /= sum; r /= sum; }
            uL[i] = (float) l; uR[i] = (float) r;
        }
        oversampler->processSamplesDown (sub);
    }

    solo.process (work.getWritePointer (0), work.getWritePointer (1), num);

    // output trim, bypass, meters
    const int numOut = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());
    float* wa = work.getWritePointer (0);
    float* wb = work.getWritePointer (1);
    const float* da = dryBuffer.getReadPointer (0);
    const float* db = dryBuffer.getReadPointer (1);
    float pk[2] { 0.0f, 0.0f };
    const double dt = 1.0 / hostRate;
    const double vuScale = 1.0 / (0.6366197723675814 * std::pow (10.0, -18.0 / 20.0));   // 0 VU = −18 dBFS sine
    float mono[1024];
    int mono_n = 0;
    for (int i = 0; i < num; ++i)
    {
        const float g = outGain.getNextValue();
        const float m = wetMix.getNextValue();
        // fully bypassed: the delayed input itself (bit-exact, whatever the circuit does)
        const float l = m == 0.0f ? da[i] : da[i] + m * (g * wa[i] - da[i]);
        const float r = m == 0.0f ? db[i] : db[i] + m * (g * wb[i] - db[i]);
        wa[i] = l; wb[i] = r;
        pk[0] = juce::jmax (pk[0], std::abs (l));
        pk[1] = juce::jmax (pk[1], std::abs (r));
        const double x[2] { std::abs ((double) l) * vuScale, std::abs ((double) r) * vuScale };
        for (int c = 0; c < 2; ++c)
        {
            auto& s = vuState[c];
            const double acc = vuW * vuW * (x[c] - s.y) - 2.0 * vuZ * vuW * s.v;
            s.v += acc * dt;
            s.y += s.v * dt;
        }
        mono[mono_n++] = 0.5f * (l + r);
        if (mono_n == 1024) { anaOut.push (mono, mono_n); mono_n = 0; }
    }
    if (mono_n > 0) anaOut.push (mono, mono_n);

    if (numOut == 1)
        buffer.copyFrom (0, start, work, 0, 0, num);
    else
        for (int ch = 0; ch < numOut; ++ch)
            buffer.copyFrom (ch, start, work, juce::jmin (ch, 1), 0, num);

    for (int c = 0; c < 2; ++c)
    {
        vu[(size_t) c].store ((float) vuState[c].y);
        float prev = peak[(size_t) c].load();
        while (pk[c] > prev && ! peak[(size_t) c].compare_exchange_weak (prev, pk[c])) {}
    }
}

void VT4AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VT4AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* VT4AudioProcessor::createEditor()
{
    return new VT4AudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VT4AudioProcessor();
}
