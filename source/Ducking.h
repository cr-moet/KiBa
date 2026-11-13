#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Tempo synced ducking curves. 'phase' runs 0 ... 1 over one period (the kick at 0);
// the shape gives 1 = no ducking ... 0 = fully ducked. The gain is
//      1 - depth * (1 - shape)
// The editor draws the same functions, so the curve on screen is what is applied.
//==============================================================================
namespace Ducking
{
    enum class Shape { pump, sharp, smooth, linear, soft, hold };
    inline const juce::StringArray shapeNames { "PUMP", "SHARP", "SMOOTH", "LINEAR", "SOFT", "HOLD" };

    // Period lengths in quarter notes, for the parameter choices
    inline const juce::StringArray rateNames { "1/16", "1/8", "1/4", "1/2", "1 BAR" };
    inline constexpr double rateBeats[] { 0.25, 0.5, 1.0, 2.0, 4.0 };

    inline float shape (Shape s, float phase) noexcept
    {
        const float p = phase - std::floor (phase);
        switch (s)
        {
            case Shape::pump:   return (1.0f - std::exp (-5.0f * p)) / (1.0f - std::exp (-5.0f));    // classic sidechain pump
            case Shape::sharp:  return std::pow (juce::jmin (1.0f, p / 0.35f), 0.7f);                 // short dip, quickly back
            case Shape::smooth: return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * p); // round, no edges
            case Shape::linear: return p;                                                             // straight ramp back
            case Shape::soft:   return p < 0.06f ? 1.0f - p / 0.06f                                  // short fade down,
                                                 : (1.0f - std::exp (-4.0f * (p - 0.06f) / 0.94f)) / (1.0f - std::exp (-4.0f));   // slow return
            case Shape::hold:   return p < 0.25f ? 0.0f : juce::jmin (1.0f, (p - 0.25f) / 0.15f);    // held down, then up
        }
        return 1.0f;
    }

    inline float gain (Shape s, float phase, float depth) noexcept
    {
        return 1.0f - depth * (1.0f - shape (s, phase));
    }
}
