#version 460
#include "path_state.glsl"

// Paths that missed geometry: add the environment and retire them.
//
// A constant sky plus a solar disc. This is a placeholder for UsdLux lighting,
// which arrives with the Hydra delegate; it is deliberately simple rather than
// approximate, so nothing here has to be unlearned when real lights land.

layout(local_size_x = 64) in;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= counters.activeCount)
    {
        return;
    }
    uint path = activeQueue.values[slot];
    if (hits.values[path].x >= 0)
    {
        return;   // hit geometry; the shade kernel owns this path
    }

    vec3 direction = pathDirection.values[path];
    vec3 radiance = frame.environmentColor.rgb;

    // The sun as a disc of finite angular radius, so it can be hit by a
    // scattered ray as well as sampled directly.
    float cosAngle = dot(direction, normalize(frame.sunDirection.xyz));
    if (cosAngle > cos(max(frame.sunDirection.w, 1.0e-4)))
    {
        radiance += frame.sunRadiance.rgb;
    }

    pathRadiance.values[path] += pathThroughput.values[path] * radiance;
    pathThroughput.values[path] = vec3(0.0);
}
