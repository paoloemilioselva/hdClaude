#version 460
#include "path_state.glsl"

// Fold each path's accumulated radiance into the film.
//
// Everything the path gathered is already in its radiance: emission, the
// environment, and every unoccluded shadow contribution. This kernel only folds
// that into the film and counts the sample.

layout(local_size_x = 64) in;

void main()
{
    uint path = gl_GlobalInvocationID.x;
    if (path >= frame.pathCount)
    {
        return;
    }
    uint pixel = pathPixel.values[path];
    vec3 radiance = pathRadiance.values[path];

    // No atomic: one path per pixel, so this invocation owns the entry. See the
    // note on the film buffer in path_state.glsl.
    accumulation.values[pixel] += vec4(radiance, 1.0);
}
