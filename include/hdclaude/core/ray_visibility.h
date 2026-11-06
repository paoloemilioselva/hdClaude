// Per-ray-kind visibility: which kinds of ray meet a placement.
//
// RenderMan and Karma both let a gprim be hidden from some kinds of ray and
// not others, through constant primvars the gprim or an ancestor authors
// (roadmap phase 20):
//
//   primvars:ri:attributes:visibility:camera        int, default 1
//   primvars:ri:attributes:visibility:indirect      int, default 1
//   primvars:ri:attributes:visibility:transmission  int, default 1 (shadows)
//   primvars:karma:object:rendervisibility          string, default "*"
//
// Both conventions are read, and a placement is hidden from a kind if either
// says so. This header is the dependency-free half: the kinds, and Karma's
// pattern. The Hydra adapters read the primvars (src/hydra/ray_visibility.h)
// and the TLAS instance mask carries the result (scene.h, InstanceMask).

#ifndef HDCLAUDE_CORE_RAY_VISIBILITY_H
#define HDCLAUDE_CORE_RAY_VISIBILITY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hdclaude {

/// The kinds of ray a placement can be hidden from. Camera rays are the ones
/// leaving the camera, until their first scattering event. Indirect rays are
/// every scattered ray after that, and the specular guide's probe. Shadow rays
/// are next-event estimation's.
inline constexpr std::uint8_t kRayCamera = 0x1;
inline constexpr std::uint8_t kRayIndirect = 0x2;
inline constexpr std::uint8_t kRayShadow = 0x4;
inline constexpr std::uint8_t kRayAll = kRayCamera | kRayIndirect | kRayShadow;

/// Karma's `rendervisibility` pattern, read.
struct KarmaRenderVisibility {
    /// The kinds the pattern leaves the placement visible to.
    std::uint8_t visibility = kRayAll;
    /// What in the pattern is not honoured as written, one line each.
    std::vector<std::string> reports;
};

/// Reads a Karma `rendervisibility` pattern.
///
/// Terms are separated by white space or commas. `*` adds every kind, a name
/// (or `+name`) adds that kind, and `-name` (or `^name`) takes it away, in
/// order. The set starts empty, unless the first term takes a kind away: then
/// it starts from every kind, so that `-shadow` means "every kind but shadow",
/// the reading salta's converter writes and open question 14 is to confirm
/// against Solaris. The names are `primary` (camera), `shadow`, and `diffuse`,
/// `reflect` and `refract`, which hdClaude does not yet tell apart: the
/// placement is visible to indirect rays if it is visible to any of the
/// three, and a pattern that separates them is reported. An empty pattern
/// and unknown names are reported and read as visible.
KarmaRenderVisibility ParseKarmaRenderVisibility(std::string_view pattern);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_RAY_VISIBILITY_H
