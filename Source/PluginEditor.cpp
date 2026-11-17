#include "PluginEditor.h"

using namespace vt4ui;

VT4AudioProcessorEditor::VT4AudioProcessorEditor (VT4AudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), faceplate (p.apvts), screen (p, model), rear (p.apvts)
{
    const double t0 = vt4diag::nowMs();
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    content.addAndMakeVisible (faceplate);
    content.addAndMakeVisible (screen);
    content.addChildComponent (rear);

    faceplate.onRear = [this] { setRearShown (! rearShown); };
    faceplate.onScreenToggle = [this] { setScreenOpen (! screenOpen); };
    faceplate.onCopyAtoB = [this] { copyAtoB(); };

    screenOpen = (bool) proc.apvts.state.getProperty ("screenOpen", true);
    const double scale = juce::jlimit (0.55, 1.6, (double) proc.apvts.state.getProperty ("uiScale", 0.82));
    screen.setVisible (screenOpen);
    faceplate.screenButton.setToggleState (screenOpen, juce::dontSendNotification);

    constrainer = std::make_unique<juce::ComponentBoundsConstrainer>();
    const int h = layout::faceH + (screenOpen ? layout::screenH : 0);
    constrainer->setFixedAspectRatio ((double) layout::W / (double) h);
    constrainer->setSizeLimits ((int) (layout::W * 0.5), (int) (h * 0.5), (int) (layout::W * 1.6), (int) (h * 1.6));
    setConstrainer (constrainer.get());
    setResizable (true, true);
    setSize ((int) std::lround (layout::W * scale), (int) std::lround (h * scale));
    startTimerHz (40);
    proc.diag().uiOpen (true);
    vt4diag::log ("[#" + juce::String (proc.diag().id()) + "] editor opened in " + juce::String (vt4diag::nowMs() - t0, 1) + " ms");
}

VT4AudioProcessorEditor::~VT4AudioProcessorEditor()
{
    stopTimer();
    proc.diag().uiOpen (false);
    vt4diag::log ("[#" + juce::String (proc.diag().id()) + "] editor closing");
    proc.auditionMode.store (0);
    setLookAndFeel (nullptr);
}

// Windows: draw with JUCE's software renderer. JUCE 8 defaults to Direct2D
// (the GPU); on some graphics drivers that can stall the whole desktop
// (host and mouse) when the window is hidden, shown or redrawn heavily.
// The interface is drawn from cached images, so the CPU renderer is cheap.
void VT4AudioProcessorEditor::chooseRenderer()
{
   #if JUCE_WINDOWS
    if (auto* peer = getPeer())
        if (peer != rendererPeer)
        {
            rendererPeer = peer;
            const auto engines = peer->getAvailableRenderingEngines();
            const int sw = engines.indexOf ("Software Renderer");
            if (sw >= 0 && peer->getCurrentRenderingEngine() != sw)
            {
                peer->setCurrentRenderingEngine (sw);
                vt4diag::log ("[#" + juce::String (proc.diag().id()) + "] editor: software renderer (was " + engines.joinIntoString ("/") + ")");
            }
        }
   #endif
}

void VT4AudioProcessorEditor::parentHierarchyChanged()
{
    chooseRenderer();
}

void VT4AudioProcessorEditor::paint (juce::Graphics& g)
{
    lnf.use();
    g.fillAll (juce::Colour (0xff121315));
}

void VT4AudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) layout::W;
    const int h = layout::faceH + (screenOpen ? layout::screenH : 0);
    content.setBounds (0, 0, layout::W, h);
    content.setTransform (juce::AffineTransform::scale (scale));
    faceplate.setBounds (0, 0, layout::W, layout::faceH);
    screen.setBounds (0, layout::faceH, layout::W, layout::screenH);
    rear.setBounds (0, layout::headerH + 2, layout::W, layout::stripY - layout::headerH - 6);
    proc.apvts.state.setProperty ("uiScale", (double) scale, nullptr);
}

void VT4AudioProcessorEditor::setScreenOpen (bool open)
{
    screenOpen = open;
    screen.setVisible (open);
    faceplate.screenButton.setToggleState (open, juce::dontSendNotification);
    proc.apvts.state.setProperty ("screenOpen", open, nullptr);
    const int h = layout::faceH + (open ? layout::screenH : 0);
    const float scale = (float) getWidth() / (float) layout::W;
    constrainer->setFixedAspectRatio ((double) layout::W / (double) h);
    constrainer->setSizeLimits ((int) (layout::W * 0.5), (int) (h * 0.5), (int) (layout::W * 1.6), (int) (h * 1.6));
    setSize (getWidth(), (int) std::lround (h * scale));
    resized();
}

void VT4AudioProcessorEditor::setRearShown (bool shown)
{
    rearShown = shown;
    rear.setVisible (shown);
    if (shown) rear.toFront (false);
    faceplate.rearButton.setButtonText (shown ? "FRONT" : "REAR");
    faceplate.rearButton.setToggleState (shown, juce::dontSendNotification);
}

void VT4AudioProcessorEditor::copyAtoB()
{
    using namespace vt4p;
    std::vector<std::pair<juce::String, juce::String>> pairs { { ids::hpf (0), ids::hpf (1) }, { ids::weight (0), ids::weight (1) }, { ids::focus (0), ids::focus (1) } };
    for (int b = 0; b < 4; ++b)
    {
        pairs.push_back ({ ids::freq (0, b), ids::freq (1, b) });
        pairs.push_back ({ ids::gain (0, b), ids::gain (1, b) });
    }
    for (auto& pr : pairs)
    {
        auto* a = proc.apvts.getParameter (pr.first);
        auto* b = proc.apvts.getParameter (pr.second);
        if (a == nullptr || b == nullptr) continue;
        b->beginChangeGesture();
        b->setValueNotifyingHost (a->getValue());
        b->endChangeGesture();
    }
}

void VT4AudioProcessorEditor::updateAudition (const vt4::DeviceParams& p)
{
    int mode = 0, set = 0, band = 0;
    screen.audition (mode, set, band);
    if (mode == 1 && model.ready())
    {
        const bool changed = mode != lastAudMode || set != lastAudSet || band != lastAudBand || ! vt4p::sameParams (p, lastSoloParams);
        if (changed)
        {
            const auto w = model.soloWindow (set, band);
            proc.soloType.store (w.type);
            proc.soloF1.store ((float) w.f1);
            proc.soloF2.store ((float) w.f2);
            lastSoloParams = p;
            screen.repaint();
        }
    }
    proc.auditionSet.store (set);
    proc.auditionBand.store (band);
    proc.auditionMode.store (mode);
    lastAudMode = mode; lastAudSet = set; lastAudBand = band;
}

void VT4AudioProcessorEditor::timerCallback()
{
    const double tIn = vt4diag::nowMs();
    if (lastTick > 0.0 && tIn - lastTick > 300.0)
        vt4diag::log ("[#" + juce::String (proc.diag().id()) + "] message thread was blocked for " + juce::String ((int) (tIn - lastTick)) + " ms");
    lastTick = tIn;
    proc.diag().uiTick();
    chooseRenderer();
    struct Took
    {
        VT4AudioProcessorEditor& e; double t0;
        ~Took()
        {
            const double ms = vt4diag::nowMs() - t0;
            if (ms > 30.0) vt4diag::log ("[#" + juce::String (e.proc.diag().id()) + "] editor timer took " + juce::String (ms, 1) + " ms");
        }
    } took { *this, tIn };
    // The display model follows the processor's sample rate and unit. A new
    // one is built on a background thread; the screen keeps the present
    // curve meanwhile, so the editor never stalls.
    const auto p = proc.currentParams();
    if (proc.preparedCount.load() > 0)
    {
        DisplayModelBuilder::Key key;
        key.osRate = proc.getOversampledRate(); key.hostRate = proc.getHostRate(); key.osOrder = proc.getOversamplingOrder();
        key.serial = p.serial; key.tolerance = p.tolerance; key.matched = p.matched;
        if (key != requestedKey)
        {
            if (! (model.ready() && key == modelKey))
                modelBuilder.request (key, p, ! model.ready() || key.osRate != modelKey.osRate || key.hostRate != modelKey.hostRate);
            // (otherwise: back to the present model)
            requestedKey = key;
        }
        DisplayModelBuilder::Key builtKey;
        if (auto built = modelBuilder.take (builtKey))
            if (builtKey == requestedKey)
            {
                model = std::move (*built);
                modelKey = builtKey;
                lastAudMode = -1;                   // SOLO window from the new model
                screen.repaint();
            }
    }
    if (model.ready() && model.sameUnit (p) && model.update (p))
        screen.repaint();

    // model: VT-4 circuit, MPEQ or PSEQ — theme, curves and which controls apply
    const int refModel = proc.pp.readModel();
    if (refModel != lnf.model)
    {
        lnf.model = refModel;
        lnf.use();
        faceplate.setModel (refModel);
        screen.repaint();
        rear.repaint();
    }
    if (model.ready() && model.setRefModel (refModel))
        screen.repaint();

    faceplate.setStereoMode (p.stereo);
    faceplate.setStepped (proc.pp.stepped->load() > 0.5f);
    faceplate.setSerial ((int) p.serial);

    // meters
    const float pkL = proc.peak[0].exchange (0.0f), pkR = proc.peak[1].exchange (0.0f);
    if (pkL > 0.944f) peakHoldL = 40;
    if (pkR > 0.944f) peakHoldR = 40;
    peakHoldL = juce::jmax (0, peakHoldL - 1);
    peakHoldR = juce::jmax (0, peakHoldR - 1);
    const float st = proc.stress.exchange (0.0f);
    stressLamp = juce::jmax (juce::jlimit (0.0f, 1.0f, st), stressLamp * 0.88f);
    const bool active = proc.pp.bypass->load() < 0.5f;
    faceplate.setMeters (proc.vu[0].load(), proc.vu[1].load(), peakHoldL > 0, peakHoldR > 0,
                         proc.bplus.load(), proc.bplusNominal.load(), stressLamp, active);
    rear.setWarmth (proc.warmth.load());

    screen.tick();
    updateAudition (p);
}
