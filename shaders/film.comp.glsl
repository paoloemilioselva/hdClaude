#version 460
#include "path_state.glsl"

// Fold each path's accumulated radiance into the film.
//
// Everything the path gathered is already in its radiance: emission, the
// environment, and every unoccluded shadow contribution. This kernel is where
// four wavelengths become a colour, and it is the only place in the renderer
// that RGB appears on the way out.
//
// The estimator, written out: XYZ is the integral of L(lambda) times the colour
// matching functions, and the packet estimates it by averaging its lanes, each
// divided by the density it was drawn with. That density is the *hero's* for
// every lane -- a rotated lane is the hero shifted and wrapped, which is a
// bijection with unit Jacobian, so its density at the value it took is the
// hero's own. Dividing by each lane's own density instead is biased by about
// forty per cent, silently, and converges just as smoothly.
//
// The normalisation is the illuminant's luminous integral, matching the
// upsampling on the way in, so a white surface under a white light resolves to
// white rather than to whatever the illuminant's absolute power happens to be.

layout(local_size_x = 64) in;

void main()
{
    uint path = gl_GlobalInvocationID.x;
    if (path >= frame.pathCount)
    {
        return;
    }
    uint pixel = pathPixel.values[path];
    vec4 radiance = pathRadiance.values[path];
    vec4 lambda = pathWavelengths.values[path];

    float density = hdclaude_wavelength_pdf(lambda.x);
    vec3 xyz = vec3(0.0);
    if (density > 0.0)
    {
        for (int lane = 0; lane < 4; ++lane)
        {
            xyz += hdclaude_spectral_row(lambda[lane]).xyz * radiance[lane];
        }
        xyz *= frame.spectralNormalisation / (4.0 * density);
    }

    // XYZ (D65-adapted) to linear sRGB, the same matrix `XyzToLinearSrgb` uses.
    vec3 rgb = vec3(
        3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
        -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
        0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z);

    // No atomic: one path per pixel, so this invocation owns the entry. See the
    // note on the film buffer in path_state.glsl.
    accumulation.values[pixel] += vec4(rgb, 1.0);
}
