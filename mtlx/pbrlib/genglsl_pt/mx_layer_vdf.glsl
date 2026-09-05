// hdClaude genglsl_pt override of layer_vdf.
//
// MaterialX 1.39.3's body, plus the sampling passthrough.
//
// A VDF layered under a surface contributes no direction of its own: the medium
// is entered through the surface, and the interior transport happens along the
// resulting ray. So the sampled direction, the density, and the delta flag all
// come from the top BSDF unchanged, and the medium parameters the VDF recorded
// travel separately through the ABI globals.

#include "lib/mx_closure_type.glsl"

void mx_layer_vdf(ClosureData closureData, BSDF top, BSDF base, out BSDF result)
{
    result.response = top.response + base.response;
    result.throughput = top.throughput + base.throughput;

    // ---- hdClaude ----------------------------------------------------------
    result.pdf = top.pdf;
    result.sampledL = top.sampledL;
    result.isDelta = top.isDelta;
    result.spectrum = top.spectrum + base.spectrum;
    result.guideAlbedo = top.guideAlbedo;
    result.guideRoughness = top.guideRoughness;
}
