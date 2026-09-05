// hdClaude genglsl_pt override of anisotropic_vdf.
//
// Upstream's genglsl body is empty:
//
//     // TODO: Add some approximation for volumetric light absorption.
//
// which is the honest thing for a rasteriser to do. A path tracer can do the
// real thing, but not *here*: volumetric absorption and scattering are
// integrated along a ray inside the medium, by the integrator, not evaluated at
// a surface by a closure.
//
// So this node's job is to publish the medium's parameters. The `shade` kernel
// reads them when a transmission event carries a path through the surface, and
// applies Beer-Lambert absorption and Henyey-Greenstein scattering over the
// segment. The closure records; the integrator transports.
//
// Scope: homogeneous interior media only. Heterogeneous volumes (UsdVol) are
// not scheduled -- docs/roadmap.md, open question 4.

#include "lib/mx_closure_type.glsl"

void mx_anisotropic_vdf(ClosureData closureData, vec3 absorption, vec3 scattering, float anisotropy, inout BSDF bsdf)
{
    hdclaude_medium_absorption = absorption;
    hdclaude_medium_scattering = scattering;
    hdclaude_medium_anisotropy = anisotropy;
    hdclaude_medium_present = 1.0;
}
