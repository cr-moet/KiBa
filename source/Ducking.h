#pragma once

#include <juce_data_structures/juce_data_structures.h>

//==============================================================================
// Tempo synced ducking curve, drawn by points. 'phase' runs 0 ... 1 over one period
// (the kick at 0); the curve gives 1 = no ducking ... 0 = fully ducked, the gain is
//      1 - depth * (1 - curve)
// The first point sits at phase 0, the last at 1 (only their height can change); every
// segment has a bend (-1 ... 1): 0 = straight, negative = fast at first, positive = slow
// at first. The presets are such point lists; editing one makes it a custom curve.
// The editor draws the same function, so the curve on screen is what is applied.
//==============================================================================
namespace Ducking
{
    inline const juce::StringArray presetNames { "PUMP", "SHARP", "SMOOTH", "LINEAR", "SOFT", "HOLD" };

    // Period lengths in quarter notes, for the parameter choices
    inline const juce::StringArray rateNames { "1/16", "1/8", "1/4", "1/2", "1 BAR" };
    inline constexpr double rateBeats[] { 0.25, 0.5, 1.0, 2.0, 4.0 };

    struct Point
    {
        float x = 0.0f, y = 0.0f, bend = 0.0f;   // bend: of the segment to the next point
    };

    // 0 ... 1 to 0 ... 1, bent by an exponential
    inline float bent (float u, float bend) noexcept
    {
        const float k = bend * 8.0f;
        return std::abs (k) < 1.0e-3f ? u : std::expm1 (k * u) / std::expm1 (k);
    }

    struct Curve
    {
        std::vector<Point> points { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };

        float evaluate (float phase) const noexcept
        {
            const float p = phase - std::floor (phase);
            for (size_t i = 1; i < points.size(); ++i)
                if (p <= points[i].x || i + 1 == points.size())
                {
                    const auto& a = points[i - 1];
                    const auto& b = points[i];
                    const float width = b.x - a.x;
                    const float u = width > 1.0e-6f ? juce::jlimit (0.0f, 1.0f, (p - a.x) / width) : 1.0f;
                    return a.y + (b.y - a.y) * bent (u, a.bend);
                }
            return 1.0f;
        }

        static Curve preset (int index)
        {
            Curve c;
            switch (index)
            {
                case 0:  c.points = { { 0, 0, -0.625f }, { 1, 1, 0 } };                                        break;   // pump
                case 1:  c.points = { { 0, 0, -0.3f }, { 0.35f, 1, 0 }, { 1, 1, 0 } };                          break;   // sharp
                case 2:  c.points = { { 0, 0, 0.4f }, { 0.25f, 0.5f, -0.4f }, { 0.5f, 1, 0.4f },
                                      { 0.75f, 0.5f, -0.4f }, { 1, 0, 0 } };                                     break;   // smooth
                case 3:  c.points = { { 0, 0, 0 }, { 1, 1, 0 } };                                               break;   // linear
                case 4:  c.points = { { 0, 1, 0 }, { 0.06f, 0, -0.5f }, { 1, 1, 0 } };                          break;   // soft
                default: c.points = { { 0, 0, 0 }, { 0.25f, 0, 0 }, { 0.4f, 1, 0 }, { 1, 1, 0 } };              break;   // hold
            }
            return c;
        }

        // Stored in the plugin state
        static inline const juce::Identifier treeType { "CURVE" }, pointType { "POINT" }, presetId { "preset" },
                                             xId { "x" }, yId { "y" }, bendId { "bend" };

        void writeTo (juce::ValueTree& tree) const
        {
            tree.removeAllChildren (nullptr);
            for (const auto& p : points)
                tree.appendChild (juce::ValueTree (pointType, { { xId, p.x }, { yId, p.y }, { bendId, p.bend } }), nullptr);
        }

        static Curve readFrom (const juce::ValueTree& tree)
        {
            Curve c;
            std::vector<Point> read;
            for (const auto& child : tree)
                if (child.hasType (pointType))
                    read.push_back ({ (float) child[xId], (float) child[yId], (float) child[bendId] });
            if (read.size() >= 2)
            {
                std::sort (read.begin(), read.end(), [] (const Point& a, const Point& b) { return a.x < b.x; });
                read.front().x = 0.0f;
                read.back().x = 1.0f;
                c.points = std::move (read);
            }
            return c;
        }
    };

    // The curve sampled for the audio thread (linear interpolation between the values)
    struct Table
    {
        static constexpr int size = 2048;
        std::array<float, size + 1> values {};

        void fill (const Curve& c) noexcept
        {
            for (int i = 0; i < size; ++i)
                values[(size_t) i] = c.evaluate ((float) i / (float) size);
            values[(size_t) size] = c.points.back().y;   // the end of the period (before the jump back)
        }

        float at (float phase) const noexcept
        {
            const float p = (phase - std::floor (phase)) * (float) size;
            const int i = juce::jlimit (0, size - 1, (int) p);
            return values[(size_t) i] + (values[(size_t) i + 1] - values[(size_t) i]) * (p - (float) i);
        }
    };
}
