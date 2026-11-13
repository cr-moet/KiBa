#pragma once

#include "PluginProcessor.h"
#include "Style.h"

//==============================================================================
// A static, opaque layer (background, display, grid, labels) rendered once into an
// image at the physical pixel scale and only redrawn when the size or the scale changes;
// the render function must fill the whole area
//==============================================================================
class CachedLayer
{
public:
    template <typename Render>
    void draw (juce::Graphics& g, juce::Rectangle<int> bounds, Render&& render)
    {
        const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const int w = juce::roundToInt ((float) bounds.getWidth() * scale), h = juce::roundToInt ((float) bounds.getHeight() * scale);
        if (w <= 0 || h <= 0)
            return;
        if (image.getWidth() != w || image.getHeight() != h || ! juce::approximatelyEqual (scale, imageScale))
        {
            image = juce::Image (juce::Image::RGB, w, h, false);   // opaque: drawn as a plain copy
            imageScale = scale;
            juce::Graphics ig (image);
            ig.addTransform (juce::AffineTransform::scale (scale));
            render (ig);
        }
        drawAtScale (g, image, imageScale, bounds.getPosition().toFloat());
    }

    void clear()   { image = {}; }

    // An image made at 'scale' back 1:1 onto the physical pixels (no resampling)
    static void drawAtScale (juce::Graphics& g, const juce::Image& image, float scale, juce::Point<float> origin)
    {
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale).translated (origin));
    }

private:
    juce::Image image;
    float imageScale = 0.0f;
};

//==============================================================================
// Oscilloscope over a window of the bar grid: kick (black) and bass (red) as min / max
// envelopes per column, the ducking gain in orange (top = 0 dB, bottom = silent) and a
// thin play line. Drag sideways to shift the curve in time, up / down for the depth;
// double click: no shift.
//==============================================================================
class ScopeView final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    struct Settings
    {
        double windowStart = 0.0, windowLength = 1.0, barLength = 4.0, period = 1.0;   // quarter notes
        Ducking::Shape shape = Ducking::Shape::pump;
        float depth = 0.6f, offset = 0.0f, zoom = 1.0f, playPosition = 0.0f;
        bool duckOn = false, sidechain = true;
    };

    std::function<void()> onDragStart, onDragEnd, onResetOffset;
    std::function<void (float offsetChange, float depthChange)> onDrag;   // in % of the period / of the depth range

    explicit ScopeView (const KiBaProcessor& p) : processor (p)   { setOpaque (true); }

    void update (const Settings& s)
    {
        const bool gridChanged = ! juce::approximatelyEqual (s.windowLength, settings.windowLength)
                              || ! juce::approximatelyEqual (s.barLength, settings.barLength);
        settings = s;
        if (gridChanged)
            layer.clear();
        repaint();
    }

    void resized() override
    {
        plot = getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (16.0f);
        layer.clear();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        lastDrag.reset();
        if (onDragStart) onDragStart();
    }
    void mouseUp (const juce::MouseEvent&) override           { if (onDragEnd) onDragEnd(); }
    void mouseDoubleClick (const juce::MouseEvent&) override  { if (onResetOffset) onResetOffset(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (onDrag == nullptr)
            return;
        const auto delta = e.getPosition() - lastDrag.value_or (e.getMouseDownPosition());
        lastDrag = e.getPosition();
        // Sideways: the window in quarter notes over the width, as % of the curve's period
        const float offsetChange = (float) delta.x / plot.getWidth() * (float) (settings.windowLength / settings.period) * 100.0f;
        const float depthChange = (float) delta.y / plot.getHeight() * 100.0f;
        onDrag (offsetChange, depthChange);
    }

    void paint (juce::Graphics& g) override
    {
        layer.draw (g, getLocalBounds(), [this] (juce::Graphics& lg) { drawStatic (lg); });
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        // Kick and bass: one shape each (upper edge forwards, lower edge back)
        const auto& columns = processor.getColumns();
        const float mid = plot.getCentreY(), scale = plot.getHeight() * 0.45f * settings.zoom;
        const float columnWidth = plot.getWidth() / (float) Scope::numColumns;
        const float limit = plot.getHeight() * 0.55f;   // clipped just outside the display
        const auto yOf = [&] (float v) { return mid - juce::jlimit (-limit, limit, v * scale); };
        const auto envelope = [&] (auto low, auto high)
        {
            juce::Path p;
            for (int c = 0; c < Scope::numColumns; ++c)
            {
                const float x = plot.getX() + ((float) c + 0.5f) * columnWidth;
                if (c == 0) p.startNewSubPath (x, yOf (high (columns[(size_t) c]))); else p.lineTo (x, yOf (high (columns[(size_t) c])));
            }
            for (int c = Scope::numColumns; --c >= 0;)
                p.lineTo (plot.getX() + ((float) c + 0.5f) * columnWidth, yOf (low (columns[(size_t) c])));
            p.closeSubPath();
            return p;
        };
        const auto load = [] (const std::atomic<float>& a) { return a.load (std::memory_order_relaxed); };
        const auto kick = envelope ([&] (const Scope::Column& c) { return load (c.kickMin); }, [&] (const Scope::Column& c) { return load (c.kickMax); });
        const auto bass = envelope ([&] (const Scope::Column& c) { return load (c.bassMin); }, [&] (const Scope::Column& c) { return load (c.bassMax); });

        // Bass below, the kick on top (it is the reference to align to)
        g.setColour (Colours::accent.withAlpha (0.45f));
        g.fillPath (bass);
        g.setColour (Colours::accent);
        g.strokePath (bass, juce::PathStrokeType (1.0f));
        g.setColour (Colours::text.withAlpha (0.4f));
        g.fillPath (kick);
        g.setColour (Colours::text.withAlpha (0.9f));
        g.strokePath (kick, juce::PathStrokeType (1.2f));

        // Ducking curve (faint while ducking is off)
        juce::Path curve;
        const int steps = juce::jmax (2, juce::roundToInt (plot.getWidth() / 2.0f));
        for (int i = 0; i <= steps; ++i)
        {
            const float fx = (float) i / (float) steps;
            const double q = settings.windowStart + fx * settings.windowLength;
            const float gain = Ducking::gain (settings.shape, (float) (q / settings.period - settings.offset), settings.depth);
            const juce::Point<float> pt { plot.getX() + fx * plot.getWidth(), plot.getY() + 6.0f + (1.0f - gain) * (plot.getHeight() - 12.0f) };
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.setColour (Colours::orange.withAlpha (settings.duckOn ? 1.0f : 0.4f));
        g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Play position
        const float px = plot.getX() + settings.playPosition * plot.getWidth();
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (px), plot.getY(), plot.getBottom());

        if (! settings.sidechain)
        {
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText ("NO SIDECHAIN - ROUTE THE KICK TO THE SIDECHAIN INPUT", plot.reduced (10.0f).removeFromBottom (16.0f), juce::Justification::centredRight);
        }
    }

private:
    void drawStatic (juce::Graphics& g) const
    {
        g.fillAll (Colours::background);
        Plot::drawDisplay (g, plot);

        // Grid: the finest step of 1/64 ... 4 bars that gives at most 16 lines;
        // bars strong, beats medium
        const double length = settings.windowLength;
        double step = 0.0625;
        while (length / step > 16.0 + 1.0e-9)
            step *= 2.0;
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (int i = 0; i * step <= length + 1.0e-9; ++i)
        {
            const double q = i * step;
            const float x = plot.getX() + (float) (q / length) * plot.getWidth();
            const bool bar = std::fmod (q + 1.0e-9, settings.barLength) < 1.0e-6;
            const bool beatLine = std::fmod (q + 1.0e-9, 1.0) < 1.0e-6;
            g.setColour (bar ? Colours::text.withAlpha (0.4f) : beatLine ? Colours::text.withAlpha (0.25f) : Colours::grid);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());

            // Labels below: bar.beat for windows of a bar or more, else the time in notes
            juce::String label;
            if (length >= settings.barLength - 1.0e-9)
            {
                if (beatLine && (length <= 2.0 * settings.barLength || bar))
                    label = juce::String ((int) (q / settings.barLength) + 1) + (bar ? juce::String() : "." + juce::String ((int) std::fmod (q, settings.barLength) + 1));
            }
            else if (i > 0 && i % 2 == 0)
                label = juce::String (q, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " Q";
            if (label.isNotEmpty() && x < plot.getRight() - 4.0f)
            {
                g.setColour (Colours::text.withAlpha (0.6f));
                g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 12.0f), juce::Justification::centred);
            }
        }
        g.setColour (Colours::text.withAlpha (0.3f));
        g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX(), plot.getRight());

        // Legend
        auto legend = plot.reduced (10.0f, 8.0f).removeFromTop (14.0f);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        for (auto [colour, text] : { std::pair { Colours::text, "KICK" }, std::pair { Colours::accent, "BASS" }, std::pair { Colours::orange, "DUCK" } })
        {
            g.setColour (colour);
            g.fillRect (legend.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 4.0f));
            legend.removeFromLeft (5.0f);
            g.setColour (Colours::text.withAlpha (0.75f));
            g.drawText (text, legend.removeFromLeft (44.0f), juce::Justification::centredLeft);
        }
    }

    const KiBaProcessor& processor;
    Settings settings;
    juce::Rectangle<float> plot;
    CachedLayer layer;
    std::optional<juce::Point<int>> lastDrag;
};
