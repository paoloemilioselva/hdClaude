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
