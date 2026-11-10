#pragma once
#include "ArcNrSyclApi.h"

namespace SnrControls
{
// MLX-DLSS 0ca2dea: StyleIndex / 128. Absence of automatic masking is
// encoded by BOTH structure selectors being -1, not by a zero strength.
inline void Set(ArcNrSyclFrame& frame, uint32_t styleIndex, float tone, float structure, float skin, bool automaticMask,
                float intensity)
{
    frame.normalized_style = static_cast<float>(styleIndex) / 128.0f;
    frame.local_tone = tone;
    frame.local_structure = structure;
    frame.skin_structure = automaticMask ? skin : -1.0f;
    frame.automatic_structure = automaticMask ? 1.0f : -1.0f;
    frame.intensity = intensity;
}
inline bool Equal(const ArcNrSyclFrame& a, const ArcNrSyclFrame& b)
{
    return a.normalized_style == b.normalized_style && a.local_tone == b.local_tone &&
           a.local_structure == b.local_structure && a.skin_structure == b.skin_structure &&
           a.automatic_structure == b.automatic_structure && a.intensity == b.intensity;
}
} // namespace SnrControls
