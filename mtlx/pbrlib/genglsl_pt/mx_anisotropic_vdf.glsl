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
// They are recorded *on the result* rather than in a global, because
// `open_pbr_surface` instantiates this node whether or not the material
// transmits anything: a global would say every OpenPBR surface in the scene
// encloses a volume. The medium belongs to the lobe that carries a path
// through the interface, and the combinators are what know which lobe that was.
// See mtlx/pbrlib/genglsl_pt/lib/mx_closure_type.glsl.
//
// Scope: homogeneous interior media only. Heterogeneous volumes (UsdVol) are
// not scheduled -- docs/roadmap.md, open question 4.

#include "lib/mx_closure_type.glsl"

void mx_anisotropic_vdf(ClosureData closureData, vec3 absorption, vec3 scattering, float anisotropy, inout BSDF bsdf)
{
    // The volume contributes nothing *at* the surface -- there is no distance to
    // integrate over at a point -- and it is zeroed rather than left alone
    // because the parameter is `inout`. An untouched `inout` carries whatever
    // the generated code declared, and a layer that adds it is adding
    // uninitialised state: `layer(dielectric_bsdf, anisotropic_vdf)` rendered
    // black for exactly that reason.
    bsdf.response = vec3(0.0);
    bsdf.throughput = vec3(0.0);
    bsdf.spectrum = vec4(0.0);
    bsdf.sampledL = vec3(0.0);
    bsdf.pdf = 0.0;
    bsdf.isDelta = 0.0;
    bsdf.guideAlbedo = vec3(0.0);
    bsdf.guideRoughness = 0.0;

    hdclaude_publish_medium(bsdf, absorption, scattering, anisotropy);
}
