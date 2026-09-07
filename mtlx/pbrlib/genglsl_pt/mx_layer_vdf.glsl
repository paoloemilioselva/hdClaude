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
    // The surface is unchanged by what it encloses. Stock MaterialX folds the
    // medium into the layer's throughput as `exp(-absorption)` -- absorption
    // over one unit of distance, evaluated at a point, which is the
    // approximation a path tracer exists to avoid. hdClaude's volume node
    // publishes the coefficient instead and the integrator applies
    // Beer-Lambert over the flight it actually measures, so there is nothing
    // for the base to add here and adding it would double the medium or, when
    // the base is untouched, corrupt the surface.
    result = top;
}
