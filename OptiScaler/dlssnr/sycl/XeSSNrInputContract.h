#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>

// Pure coordinate/exposure conversion for SNR BEFORE XeSS. Never write these
// converted values back into the game's XeSS execute parameters.
namespace XeSSNr {
struct Vec2 { float x = 0, y = 0; };
struct MotionTransform { Vec2 scale, bias; };

inline MotionTransform MotionToRenderPixels(unsigned width, unsigned height,
    Vec2 velocityScale, bool ndc, bool jitteredMotion,
    Vec2 currentGeometryJitter, Vec2 previousGeometryJitter) {
    if (!width || !height || !std::isfinite(velocityScale.x) ||
        !std::isfinite(velocityScale.y) || !std::isfinite(currentGeometryJitter.x) ||
        !std::isfinite(currentGeometryJitter.y) || !std::isfinite(previousGeometryJitter.x) ||
        !std::isfinite(previousGeometryJitter.y))
        throw std::invalid_argument("invalid native XeSS motion contract");
    MotionTransform result {{velocityScale.x, velocityScale.y}, {}};
    if (ndc) {
        result.scale.x *= .5f * width;
        result.scale.y *= -.5f * height;
    }
    // XeSS Jx/Jy describe geometry displacement (sample displacement is its
    // negative). Reprojecting current jittered colour into previous jittered
    // colour therefore adds previous geometry jitter - current geometry jitter.
    if (!jitteredMotion) {
        result.bias.x = previousGeometryJitter.x - currentGeometryJitter.x;
        result.bias.y = previousGeometryJitter.y - currentGeometryJitter.y;
    }
    return result;
}

inline float ScalarWhitePoint(float exposure, float multiplier) {
    const float product = exposure * multiplier;
    if (!std::isfinite(product) || product <= 0)
        throw std::invalid_argument("invalid native XeSS exposure");
    return 1.f / product;
}
} // namespace XeSSNr
