#pragma once

// How much light a UsdLux light delivers, per unit of the luminance it emits.
//
// The draft UsdLux physical-lighting proposal (docs/usd-proposals.md) lets a
// light be authored by what it delivers -- an area light's power in lumens, or
// the illuminance it puts on a surface facing it at some distance; a distant
// or dome light's illuminance -- rather than by its luminance. A renderer meets
// that by scaling the luminance it would have emitted by the target over what
// that luminance delivers, which is the reciprocal the proposal's
// `ComputePowerScaleFactor` and `ComputeIlluminanceScaleFactor` return.
//
// What a luminance delivers depends only on the emitter's shape, size and
// UsdLuxShapingAPI cone and focus, so these take those and return the factor
// per unit luminance. They are integrals of exactly what the path tracer
// emits -- the shaping mirrors `hdclaude_light_shaping` in path_state.glsl --
// because the promise is that the light then *delivers* the target, and an
// integral of a slightly different light would break it by however much the
// two differ.
//
// Lengths are in whatever unit the caller's geometry is in. The power is in
// that unit squared times steradians, so a caller converts with the stage's
// metres per unit; the illuminance factor is a projected solid angle and has
// no unit.

namespace hdclaude {

/// The maximum luminous efficacy of radiation, lm/W: what the SI's definition
/// of the candela fixes at 540 THz, and what CIE photometry multiplies the
/// ybar-weighted radiant integral by.
inline constexpr double kMaximumLuminousEfficacy = 683.0;

/// UsdLuxShapingAPI's cone and focus as hdClaude applies them.
struct EmitterShaping {
    /// Cosine of the cone's half-angle, or -1 for an unshaped light.
    float coneCosAngle = -1.0f;
    /// 0 for a hard edge, 1 for a falloff across the whole cone.
    float coneSoftness = 0.0f;
    /// Exponent on the cosine to the emitting surface's normal, 0 for none.
    float focus = 0.0f;

    bool Shaped() const { return coneCosAngle > -1.0f || focus > 0.0f; }
};

/// The fraction of a light's luminance it emits in a direction at `cosAxis`
/// to the light's axis and `cosNormal` to the emitting surface's normal.
///
/// The host twin of `hdclaude_light_shaping`, and both are UsdLuxShapingAPI
/// as its documentation writes it: zero beyond the cone angle; inside it,
/// `1 - smoothstep(smoothStart, cutoff, offAxis)` in the off-axis *angle*,
/// with `smoothStart = cutoff * (1 - softness)`, so softness 0 is a hard edge
/// and 1 a falloff across the whole cone; and `|cosNormal|^focus`. The focus
/// is about the surface's normal, which is the axis for a rect or disk and
/// not for a sphere or cylinder.
float ShapingAttenuation(const EmitterShaping& shaping, float cosAxis, float cosNormal);

enum class EmitterShape { Rect, Disk, Sphere, Cylinder };

/// An area light as the path tracer samples it, in world space.
struct AreaEmitter {
    EmitterShape shape = EmitterShape::Rect;
    double position[3] = {0.0, 0.0, 0.0};
    /// The axis it emits along (USD's -Z), normalised.
    double direction[3] = {0.0, 0.0, -1.0};
    /// A rect's half-extent along its local X, and a cylinder's half-length
    /// along its axis.
    double uAxis[3] = {0.5, 0.0, 0.0};
    /// A rect's half-extent along its local Y.
    double vAxis[3] = {0.0, 0.5, 0.0};
    /// A disk's, sphere's or cylinder's radius.
    double radius = 0.5;
    EmitterShaping shaping;
};

/// The emitter's surface area. A cylinder has no caps, as in UsdLux.
double EmitterArea(const AreaEmitter& emitter);

/// Power emitted per unit luminance: the integral over the surface and the
/// directions each point emits into of shaping times the cosine to the
/// surface, in length^2 sr.
///
/// `pi * area` for an unshaped light. A rect or disk emits from its front face
/// only, a sphere and a cylinder from every point outward.
double EmittedPowerPerLuminance(const AreaEmitter& emitter);

/// Illuminance per unit luminance on a surface facing the light, on its axis
/// at `distance` from its centre: the integral over the light of shaping times
/// both cosines over distance squared.
///
/// Returns 0 when that surface is on or inside the light -- a sphere or a
/// cylinder at no more than its radius -- where "facing the light at a
/// distance" means nothing.
double OnAxisIlluminancePerLuminance(const AreaEmitter& emitter, double distance);

/// Illuminance per unit luminance from a distant light of angular radius
/// `angularRadius` (radians) on a surface facing it: the cone's projected
/// solid angle, `pi sin^2(theta)`, and `pi` once the cone covers the
/// surface's whole hemisphere.
double DistantIlluminancePerLuminance(double angularRadius);

}  // namespace hdclaude
