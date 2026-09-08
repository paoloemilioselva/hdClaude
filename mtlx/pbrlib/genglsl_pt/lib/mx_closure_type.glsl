// hdClaude path-tracing closure protocol.
//
// Replaces pbrlib/genglsl/lib/mx_closure_type.glsl for the `genglsl_pt` target.
// The stock content is reproduced verbatim; hdClaude only appends.
//
// Version of record: MaterialX 1.39.3, the version that ships inside OpenUSD
// 26.03 and that hdClaude links and generates with. See
// docs/materialx-codegen.md and docs/implementation-notes.md.

// --- Stock MaterialX 1.39.3, verbatim ---------------------------------------
// These are defined based on the HwShaderGenerator::ClosureContextType enum
// if that changes - these need to be updated accordingly.

#define CLOSURE_TYPE_DEFAULT 0
#define CLOSURE_TYPE_REFLECTION 1
#define CLOSURE_TYPE_TRANSMISSION 2
#define CLOSURE_TYPE_INDIRECT 3
#define CLOSURE_TYPE_EMISSION 4

struct ClosureData {
    int closureType;
    vec3 L;
    vec3 V;
    vec3 N;
    vec3 P;
    float occlusion;
};

// --- hdClaude path-tracing extension ----------------------------------------
//
// ONE added closure type, and it produces only a direction.
//
// PT_SAMPLE  choose an incident direction. Reads the sample globals below,
//            writes BSDF.sampledL and BSDF.isDelta.
//
// The density is not sampling's output. MaterialX evaluates a combinator's
// children *before* the combinator, so at combination time each child holds a
// density for its own sampled direction, and those are not densities of the
// same direction -- they cannot be mixed. Instead CLOSURE_TYPE_REFLECTION and
// CLOSURE_TYPE_TRANSMISSION write BSDF.pdf beside BSDF.response, and every
// combinator mixes densities with the weights it already mixes responses with.
//
// A scattering event is therefore:
//     pass 1  PT_SAMPLE   -> a direction
//     pass 2  REFLECTION  -> f and pdf, both at that direction
//     weight = f / pdf
//
// and next-event estimation needs only pass 2. One code path produces every
// density in the renderer, so a combinator cannot report a density that
// disagrees with the response beside it. Full reasoning in
// docs/materialx-codegen.md 2.

#define CLOSURE_TYPE_PT_SAMPLE 16

// Number of correlated hero wavelengths carried per path. Must equal
// hdclaude::kSpectralLanes in include/hdclaude/core/spectrum.h.
#define HDCLAUDE_SPECTRAL_LANES 4

// Smallest GGX alpha the estimator can evaluate, as opposed to the smallest a
// material may author. See the note at the clamp in mx_dielectric_bsdf.
const float kHdclaudeMinAlpha = 1.0e-4;

// Per-invocation state the closures read.
//
// Globals rather than extra ClosureData fields, deliberately. ClosureData is
// constructed by C++ node implementations with a fixed argument list
// (`ClosureData(CLOSURE_TYPE_X, L, V, N, P, occlusion)`), so adding a field
// would break every stock construction site that hdClaude does not itself
// replace. In a compute shader a global is per-invocation, so this costs
// nothing and keeps the struct byte-compatible with upstream.
//
// The `shade` kernel writes these before calling the material entry point.
vec4 hdclaude_wavelengths = vec4(0.0);  // hero wavelengths, nanometres
vec3 hdclaude_sample_u = vec3(0.0);     // stratified sample: xy direction, z lobe

// The *geometric* normal at the hit, unflipped, or zero where none was supplied.
//
// A closure that has to know which side of an interface it is on cannot ask the
// shading normal. On a smooth-shaded mesh the interpolated normal tilts past the
// horizon near a silhouette, so `dot(N, V) < 0` happens routinely on the outside
// of a perfectly opaque object -- and a closure reading that as "inside" lights
// up every silhouette in the scene. Only the geometric normal answers the
// question the closure is actually asking.
//
// Zero means the caller did not supply one, and a closure must fall back to the
// shading normal rather than treating the surface as edge-on.
vec3 hdclaude_geometric_normal = vec3(0.0);

// Whether the path is currently *inside* a dense medium, as the integrator
// tracks it: set by a transmission event that carried the path through a
// surface, cleared by the one that carried it back out.
//
// A closure cannot work this out for itself and must not guess. "Arriving from
// behind" is not the same question: a ray reaching the back face of an opaque
// object with a hole in it arrives from behind and is in the air, while a ray
// inside a glass ball is in glass whichever of its lobes is being asked. The
// first must see the authored index and the second its reciprocal, and only the
// path's own history separates them.
float hdclaude_inside_medium = 0.0;

/// Which side of an interface `V` is on, from the geometric normal when there is
/// one and the shading normal otherwise.
bool hdclaude_entering(vec3 shadingNormal, vec3 V)
{
    vec3 side = dot(hdclaude_geometric_normal, hdclaude_geometric_normal) > 0.5
                    ? hdclaude_geometric_normal
                    : shadingNormal;
    return dot(side, V) > 0.0;
}

// Interior medium, published by anisotropic_vdf and read by the integrator.
//
// Volumetric absorption and scattering are integrated *along a ray inside the
// medium*, not evaluated at a surface, so the closure's job is to record the
// parameters and the integrator's job is to transport with them. The kernel
// reads these when a transmission event carries a path through the surface.
vec3  hdclaude_medium_absorption = vec3(0.0);
vec3  hdclaude_medium_scattering = vec3(0.0);
float hdclaude_medium_anisotropy = 0.0;
float hdclaude_medium_present = 0.0;

// Subsurface, published by subsurface_bsdf on the same principle: the closure
// supplies the boundary condition and the medium parameters, and the integrator
// performs the bounded spectral random walk. This mirrors MaterialX's own OSL
// target, which emits a subsurface_bssrdf closure and leaves transport to the
// renderer, rather than the genglsl target's screen-space approximation.
vec3  hdclaude_subsurface_albedo = vec3(0.0);
vec3  hdclaude_subsurface_radius = vec3(0.0);   // per-channel mean free path
float hdclaude_subsurface_anisotropy = 0.0;     // Henyey-Greenstein g
float hdclaude_subsurface_present = 0.0;

// Dispersion, written by the integrator rather than published by a closure.
//
// The direction of travel is the opposite of the medium's, and it has to be.
// MaterialX 1.39.3's `open_pbr_surface` declares
// `transmission_dispersion_abbe_number`, threads it into the generated
// function's signature, and never reads it; `ND_dielectric_bsdf` has no
// dispersion input to receive it. So no closure can learn the Abbe number from
// the graph it is generated in. The material compiler reads it from the
// authored network instead and the shade kernel sets it here, per dispatch,
// beside the wavelengths it already sets (docs/implementation-notes.md,
// 2026-09-07).
//
// Zero means no dispersion, which is what every material that does not author
// it gets, and what makes this cost nothing where it is not used.
float hdclaude_dispersion_abbe = 0.0;

// The three Fraunhofer lines the Abbe number is defined against, in nanometres.
// Same values as `kFraunhoferF/D/C` in hdclaude/core/spectrum.h.
#define HDCLAUDE_FRAUNHOFER_F 486.13
#define HDCLAUDE_FRAUNHOFER_D 587.56
#define HDCLAUDE_FRAUNHOFER_C 656.27

/// Index of refraction at `lambda` nanometres, from a nominal index and an
/// Abbe number. Mirrors hdclaude::DispersedIor, and is checked against it.
///
/// Two-term Cauchy, `n = A + B / lambda^2`. Both coefficients are fixed by the
/// two numbers an asset authors: B by the Abbe number's own definition,
/// `V = (nd - 1) / (nF - nC)`, and A by the curve passing through the quoted
/// index at the d-line. A smaller Abbe number is a *more* dispersive glass.
float hdclaude_dispersed_ior(float ior, float abbe, float lambda)
{
    if (!(abbe > 0.0))
    {
        return ior;
    }
    const float inverseF = 1.0 / (HDCLAUDE_FRAUNHOFER_F * HDCLAUDE_FRAUNHOFER_F);
    const float inverseC = 1.0 / (HDCLAUDE_FRAUNHOFER_C * HDCLAUDE_FRAUNHOFER_C);
    const float inverseD = 1.0 / (HDCLAUDE_FRAUNHOFER_D * HDCLAUDE_FRAUNHOFER_D);

    float b = (ior - 1.0) / (abbe * (inverseF - inverseC));
    float a = ior - b * inverseD;

    float safe = max(1.0, lambda);
    return a + b / (safe * safe);
}

// The BSDF struct is extended by hdClaude's Syntax override rather than
// declared here, because MaterialX registers it as a type syntax in GlslSyntax
// rather than emitting it from a library file. See
// src/materialx/pathtracer_generator.cpp. For reference the extended shape is:
//
//   struct BSDF {
//       vec3  response;        // stock: f * cos, or radiance for an EDF
//       vec3  throughput;      // stock: energy left for the layer below
//       vec4  spectrum;        // response resolved on the hero wavelengths
//       vec3  sampledL;        // PT_SAMPLE output direction
//       float pdf;             // solid-angle density, from the eval types
//       float isDelta;         // specular: skip NEE, MIS weight is one
//       vec3  guideAlbedo;     // demodulation albedo for reconstruction
//       float guideRoughness;  // representative roughness for reconstruction
//   };
//
// The struct definition and its default-value expression come from the same
// Syntax registration, so they cannot drift apart. `isDelta` is a float so the
// default stays a plain aggregate literal.
