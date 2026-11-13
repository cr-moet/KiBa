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
        double tempo = 120.0;
        Ducking::Curve curve;
        std::optional<float> kickOnset;   // ms to the nearest beat
        float depth = 0.6f, offset = 0.0f, zoom = 1.0f, playPosition = 0.0f;
        bool duckOn = false, sidechain = true;
    };

    std::function<void()> onDragStart, onDragEnd;
    std::function<void (float offsetChange, float depthChange)> onDrag;   // in % of the period / of the depth range
    std::function<void (const Ducking::Curve&)> onCurveChange;             // the curve was edited

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

    //==========================================================================
    // Mouse: on a point: move it; on the curve: bend that segment (up / down);
    // elsewhere: shift the curve in time (sideways) and its depth (up / down).
    // Double click: a new point, or on a point: remove it.
    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto pos = e.position;
        setMouseCursor (hitPoint (pos) >= 0 ? juce::MouseCursor::DraggingHandCursor
                        : hitSegment (pos) >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                                : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        lastDrag.reset();
        working = settings.curve;
        dragStart = e.position;
        if ((dragIndex = hitPoint (e.position)) >= 0)
        {
            mode = Mode::point;
            dragStartX = working.points[(size_t) dragIndex].x;
        }
        else if ((dragIndex = hitSegment (e.position)) >= 0)
        {
            mode = Mode::bend;
            dragStartX = working.points[(size_t) dragIndex].bend;
        }
        else
        {
            mode = Mode::offsetDepth;
            if (onDragStart) onDragStart();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (mode == Mode::offsetDepth && onDragEnd)
            onDragEnd();
        mode = Mode::none;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& points = working.points;
        if (mode == Mode::point)
        {
            auto& p = points[(size_t) dragIndex];
            const bool end = dragIndex == 0 || dragIndex == (int) points.size() - 1;
            if (! end)
                p.x = juce::jlimit (points[(size_t) dragIndex - 1].x + 0.002f, points[(size_t) dragIndex + 1].x - 0.002f,
                                    dragStartX + (e.position.x - dragStart.x) / periodWidth());
            p.y = curveValueAt (e.position.y);
            changed();
        }
        else if (mode == Mode::bend)
        {
            auto& p = points[(size_t) dragIndex];
            const bool rising = points[(size_t) dragIndex + 1].y >= p.y;
            p.bend = juce::jlimit (-1.0f, 1.0f, dragStartX + (e.position.y - dragStart.y) / 120.0f * (rising ? 1.0f : -1.0f));
            changed();
        }
        else if (mode == Mode::offsetDepth && onDrag != nullptr)
        {
            const auto delta = e.getPosition() - lastDrag.value_or (e.getMouseDownPosition());
            lastDrag = e.getPosition();
            // Sideways: the window in quarter notes over the width, as % of the curve's period
            onDrag ((float) delta.x / plot.getWidth() * (float) (settings.windowLength / settings.period) * 100.0f,
                    (float) delta.y / plot.getHeight() * 100.0f);
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        working = settings.curve;
        auto& points = working.points;
        if (const int i = hitPoint (e.position); i > 0 && i < (int) points.size() - 1)
            points.erase (points.begin() + i);
        else if (i < 0)
        {
            const float x = phaseAt (e.position.x);
            const auto next = std::upper_bound (points.begin(), points.end(), x, [] (float v, const Ducking::Point& p) { return v < p.x; });
            if (next == points.begin() || next == points.end())
                return;
            points.insert (next, { x, curveValueAt (e.position.y), 0.0f });
        }
        changed();
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
            const float value = settings.curve.evaluate ((float) (q / settings.period - settings.offset));
            const juce::Point<float> pt { plot.getX() + fx * plot.getWidth(), yOf (value) };
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.setColour (Colours::orange.withAlpha (settings.duckOn ? 1.0f : 0.4f));
        g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // The points (when a period is wide enough to edit)
        if (periodWidth() >= 40.0f)
            for (const auto& p : settings.curve.points)
                forEachRepetition (p.x, [&] (float x)
                {
                    const auto r = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ x, yOf (p.y) });
                    g.setColour (Colours::display);
                    g.fillEllipse (r);
                    g.setColour (Colours::orange.withAlpha (settings.duckOn ? 1.0f : 0.5f));
                    g.drawEllipse (r, 2.0f);
                });

        // Play position
        const float px = plot.getX() + settings.playPosition * plot.getWidth();
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (px), plot.getY(), plot.getBottom());

        // Onsets of kick and bass relative to the nearest grid line
        {
            const auto text = [this] (bool isKick)
            {
                const auto ms = isKick ? settings.kickOnset.has_value() ? std::optional<double> (*settings.kickOnset) : std::optional<double>()
                                       : onsetMs (false);
                return juce::String (isKick ? "KICK  " : "BASS  ") + (ms ? (*ms >= 0.0 ? "+" : "") + juce::String (*ms, 1) + " ms" : juce::String ("--"));
            };
            g.setColour (Colours::text.withAlpha (0.75f));
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText ("ONSET TO GRID:  " + text (true) + "     " + text (false), plot.reduced (10.0f, 8.0f).removeFromTop (14.0f), juce::Justification::centredRight);
        }

        if (! settings.sidechain)
        {
            g.setColour (Colours::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText ("NO SIDECHAIN - ROUTE THE KICK TO THE SIDECHAIN INPUT", plot.reduced (10.0f).removeFromBottom (16.0f), juce::Justification::centredRight);
        }
    }

    // Start of the kick (or bass) relative to the nearest grid line (beat, or the window when
    // it is shorter than a beat), in ms; empty when there is no signal
    std::optional<double> onsetMs (bool kick) const
    {
        const auto& columns = processor.getColumns();
        const auto amplitude = [kick] (const Scope::Column& c)
        {
            const auto low = (kick ? c.kickMin : c.bassMin).load (std::memory_order_relaxed);
            const auto high = (kick ? c.kickMax : c.bassMax).load (std::memory_order_relaxed);
            return juce::jmax (std::abs (low), std::abs (high));
        };
        int peak = 0;
        float peakValue = 0.0f;
        for (int c = 0; c < Scope::numColumns; ++c)
            if (const float a = amplitude (columns[(size_t) c]); a > peakValue) { peakValue = a; peak = c; }
        if (peakValue < 1.0e-3f)
            return {};
        int onset = peak;
        while (onset > 0 && amplitude (columns[(size_t) onset - 1]) > 0.15f * peakValue)
            --onset;
        const double step = juce::jmin (1.0, settings.windowLength);
        const double q = settings.windowStart + (double) onset / Scope::numColumns * settings.windowLength;
        return (q - std::round (q / step) * step) * 60000.0 / settings.tempo;
    }

private:
    enum class Mode { none, point, bend, offsetDepth };

    float periodWidth() const   { return plot.getWidth() * (float) (settings.period / settings.windowLength); }

    // Curve value (0 ... 1) <-> y: the gain from 0 dB at the top to silence at the bottom
    float yOf (float value) const
    {
        const float gain = 1.0f - settings.depth * (1.0f - value);
        return plot.getY() + 6.0f + (1.0f - gain) * (plot.getHeight() - 12.0f);
    }

    float curveValueAt (float y) const
    {
        const float gain = 1.0f - (y - plot.getY() - 6.0f) / (plot.getHeight() - 12.0f);
        return juce::jlimit (0.0f, 1.0f, 1.0f - (1.0f - gain) / juce::jmax (0.05f, settings.depth));
    }

    float phaseAt (float x) const
    {
        const double q = settings.windowStart + (x - plot.getX()) / plot.getWidth() * settings.windowLength;
        const double phase = q / settings.period - settings.offset;
        return (float) (phase - std::floor (phase));
    }

    // Every x in the window where the curve is at 'phase'
    template <typename Callback>
    void forEachRepetition (float phase, Callback&& callback) const
    {
        const double first = std::floor (settings.windowStart / settings.period - settings.offset) - 1.0;
        for (double k = first; ; k += 1.0)
        {
            const double q = (k + phase + settings.offset) * settings.period;
            const float x = plot.getX() + (float) ((q - settings.windowStart) / settings.windowLength) * plot.getWidth();
            if (x > plot.getRight() + 1.0f)
                break;
            if (x >= plot.getX() - 1.0f)
                callback (x);
        }
    }

    int hitPoint (juce::Point<float> pos) const
    {
        int best = -1;
        float bestDistance = 9.0f;
        for (size_t i = 0; i < settings.curve.points.size(); ++i)
        {
            const auto& p = settings.curve.points[i];
            forEachRepetition (p.x, [&] (float x)
            {
                if (const float d = pos.getDistanceFrom ({ x, yOf (p.y) }); d < bestDistance)
                {
                    bestDistance = d;
                    best = (int) i;
                }
            });
        }
        return periodWidth() >= 40.0f ? best : -1;
    }

    int hitSegment (juce::Point<float> pos) const
    {
        const float phase = phaseAt (pos.x);
        if (std::abs (yOf (settings.curve.evaluate (phase)) - pos.y) > 7.0f)
            return -1;
        const auto& points = settings.curve.points;
        for (size_t i = 1; i < points.size(); ++i)
            if (phase <= points[i].x)
                return (int) i - 1;
        return (int) points.size() - 2;
    }

    void changed()
    {
        settings.curve = working;
        if (onCurveChange)
            onCurveChange (working);
        repaint();
    }

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
    Mode mode = Mode::none;
    Ducking::Curve working;
    juce::Point<float> dragStart;
    float dragStartX = 0.0f;
    int dragIndex = -1;
};

//==============================================================================
// Push button with the picture of a ducking preset (two periods) and the LED
//==============================================================================
class CurveButton final : public juce::TextButton
{
public:
    explicit CurveButton (int presetIndex) : curve (Ducking::Curve::preset (presetIndex))
    {
        getProperties().set ("led", true);
        setTooltip (Ducking::presetNames[presetIndex]);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        getLookAndFeel().drawButtonBackground (g, *this, findColour (buttonColourId), highlighted, down);
        auto face = KiBaLookAndFeel::buttonFaceArea (getLocalBounds().toFloat(), down || getToggleState());
        face.removeFromTop (9.0f);
        const auto area = face.reduced (12.0f, 5.0f);

        juce::Path path;
        const int steps = juce::jmax (8, (int) area.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const float fx = (float) i / (float) steps;
            const float phase = juce::jmin (fx * 2.0f, 1.999f);   // two periods
            const juce::Point<float> pt { area.getX() + fx * area.getWidth(), area.getBottom() - curve.evaluate (phase) * area.getHeight() };
            if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
        }
        g.setColour (Colours::text.withAlpha (isEnabled() ? 0.9f : 0.4f));
        g.strokePath (path, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    Ducking::Curve curve;
};
