#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vt4ui
{

// ---- palette ------------------------------------------------------------------
namespace col
{
    const juce::Colour plateTop    { 0xff2b2e33 };
    const juce::Colour plateBottom { 0xff1c1e21 };
    // themed (set by useTheme before painting): engraving and indicator colours
    inline juce::Colour brass      { 0xffd2b47a };
    inline juce::Colour brassDim   { 0xff9c8358 };
    inline juce::Colour brassFaint { 0x55c8a96b };
    const juce::Colour engraveShadow { 0xd0050506 };
    inline juce::Colour amber      { 0xffffb648 };
    inline juce::Colour amberDim   { 0xff7a5520 };
    inline juce::Colour amberText  { 0xffffd79a };     // lit key legend
    const juce::Colour green       { 0xff9be7a4 };
    const juce::Colour glass       { 0xff08090a };
    const juce::Colour cream       { 0xfff4e6c3 };
    const juce::Colour ink         { 0xff171310 };
    const juce::Colour red         { 0xffd2412f };
}

// ---- themes ------------------------------------------------------------------
// One per model: 0 VT-4 (graphite and brass), 1 MPEQ (blue steel and
// silver), 2 PSEQ (black anodised, white legends, red indicators).
struct Theme
{
    juce::Colour plateTop, plateMid, plateBottom;
    juce::Colour brass, brassDim, brassFaint, amber, amberDim, amberText;
    bool darkKnobs;
    uint32_t seed;
};
const Theme& themeFor (int model);
// sets the col:: palette for the painting that follows
void useTheme (int model);
int currentTheme();

// ---- fonts ---------------------------------------------------------------------
struct Fonts
{
    static Fonts& get();
    juce::Typeface::Ptr medium, semibold, displayLight, displayMedium;

    static juce::Font label (float h, float tracking = 0.12f);     // engraved small caps
    static juce::Font strong (float h, float tracking = 0.08f);
    static juce::Font display (float h, float tracking = 0.0f);    // light display face
    static juce::Font displayBold (float h, float tracking = 0.0f);
};

// Text that looks engraved into the faceplate and filled with brass paint.
void drawEngraved (juce::Graphics&, const juce::String&, juce::Rectangle<float>, const juce::Font&,
                   juce::Justification, juce::Colour fill = col::brass, float depth = 1.0f);

// A machined, knurled aluminium knob (no scale: that is engraved on the plate).
void drawKnob (juce::Graphics&, juce::Rectangle<float> bounds, float angle, bool large, bool enabled, float hover);

// applies the theme of the editor a component belongs to
void useThemeOf (juce::Component&);

// Live resizing: while the window is being dragged to a new size, cached
// layers are drawn scaled from the size they were rendered at (fast);
// they are rendered again, sharp, once the size has settled.
void noteLiveResize();
bool liveResizing();

// The Moon Music Label mark: a disc with three vertical slots.
void drawMoonMark (juce::Graphics&, juce::Point<float> centre, float diameter, juce::Colour);

// Brushed, anodised metal generated per physical pixel (fills the image).
void renderBrushedMetal (juce::Image&, float scale, juce::Colour top, juce::Colour mid, juce::Colour bottom, uint32_t seed);

// Draws a cached layer (rendered at `scale` physical pixels per unit) at the
// origin, in tiles of at most 256 px. Large single images can come out as
// garbage on some macOS graphics paths; small ones are reliable everywhere.
void drawCachedLayer (juce::Graphics&, const juce::Image&, float scale);

// Brushed metal drawn with vector primitives only: the vertical colour
// gradient, then a fine horizontal grain of 1-unit streaks (used on macOS).
void paintBrushedMetalVector (juce::Graphics&, juce::Rectangle<float> area, juce::Colour top, juce::Colour mid, juce::Colour bottom, uint32_t seed);

// A hairline frame engraved in a plate, interrupted where its title sits.
void drawFrame (juce::Graphics&, juce::Rectangle<float> frame, juce::Rectangle<float> gapArea, float corner, juce::Colour line);

// A jewel indicator lamp; level 0 … 1.
void drawJewel (juce::Graphics&, juce::Rectangle<float>, juce::Colour, float level);

class VT4LookAndFeel : public juce::LookAndFeel_V4
{
public:
    VT4LookAndFeel();

    int model = 0;              // theme of this editor
    void use() const { useTheme (model); }

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int h) override;
    void drawBubble (juce::Graphics&, juce::BubbleComponent&, const juce::Point<float>&, const juce::Rectangle<float>&) override;
    juce::Font getSliderPopupFont (juce::Slider&) override;
    int getSliderPopupPlacement (juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    void drawTooltip (juce::Graphics&, const juce::String&, int w, int h) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

} // namespace vt4ui
