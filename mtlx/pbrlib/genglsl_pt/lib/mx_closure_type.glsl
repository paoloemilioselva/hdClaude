// hdClaude path-tracing closure protocol.
//
// This file replaces pbrlib/genglsl/lib/mx_closure_type.glsl for the
// `genglsl_pt` target. The stock closure types keep their values and meanings
// verbatim, so every unmodified upstream closure body remains valid; hdClaude
// only *adds*.
//
// See docs/materialx-codegen.md 2.

// --- Stock MaterialX closure types, unchanged -------------------------------
// These are defined based on the HwShaderGenerator::ClosureContextType enum.
// If that changes upstream, these must change with it.

#define CLOSURE_TYPE_DEFAULT 0
#define CLOSURE_TYPE_REFLECTION 1
#define CLOSURE_TYPE_TRANSMISSION 2
#define CLOSURE_TYPE_INDIRECT 3
#define CLOSURE_TYPE_EMISSION 4

// --- hdClaude path-tracing closure type -------------------------------------
//
// Numbered well above the stock range so that a future upstream addition
// cannot silently collide with it.
//
// PT_SAMPLE  choose an incident direction. Reads ClosureData.u, writes
//            BSDF.sampledL and BSDF.isDelta.
//
// There is exactly one added closure type, and it produces *only a direction*.
// The density is not its output. This follows from how MaterialX generates a
// closure graph: a combinator such as `mix_bsdf` receives its children already
// evaluated, so at combination time each child holds a density for *its own*
// sampled direction, and those are not densities of the same direction. They
// cannot be mixed.
//
// Instead, CLOSURE_TYPE_REFLECTION and CLOSURE_TYPE_TRANSMISSION are extended
// to write BSDF.pdf alongside BSDF.response, and every combinator mixes
// densities with exactly the weights it already mixes responses with. A
// scattering event is then:
//
//   pass 1  PT_SAMPLE     -> a direction
//   pass 2  REFLECTION    -> f and pdf, both at that direction
//   weight = f / pdf
//
// and next-event estimation needs only pass 2, which yields the MIS density
// for free. One code path produces every density in the renderer, so a
// combinator cannot report a density that disagrees with the response it
// reports beside it -- which is the failure mode that makes an image subtly and
// unfixably wrong under MIS.

#define CLOSURE_TYPE_PT_SAMPLE 16

// Number of correlated hero wavelengths carried per path. Must equal
// hdclaude::kSpectralLanes in include/hdclaude/core/spectrum.h.
#define HDCLAUDE_SPECTRAL_LANES 4

struct ClosureData {
    // --- Stock fields. Names and meanings are upstream's. --------------------
    int closureType;
    vec3 L;              // incident direction; an *output* under PT_SAMPLE
    vec3 V;              // outgoing direction, toward the viewer
    vec3 N;
    vec3 P;
    float occlusion;

    // --- hdClaude path-tracing extension -------------------------------------
    vec4 wavelengths;    // the four hero wavelengths, nanometres
    vec3 u;              // stratified sample: u.xy direction, u.z lobe choice
};

// The BSDF struct itself is extended by hdClaude's Syntax override rather than
// declared here, because MaterialX registers it as a type syntax in
// GlslSyntax rather than emitting it from a library file. hdClaude's
// PathTracerSyntax re-registers Type::BSDF with these fields and a matching
// default value; see docs/materialx-codegen.md 5. For reference, the extended
// shape is:
//
//   struct BSDF {
//       vec3  response;        // stock: f * cos, or radiance for an EDF
//       vec3  throughput;      // stock: energy left for the layer below
//       vec4  spectrum;        // response resolved on the hero wavelengths
//       vec3  sampledL;        // PT_SAMPLE output direction
//       float pdf;             // solid-angle density, written by the
//                              // evaluation closure types (see above)
//       float isDelta;         // specular: skip NEE, MIS weight is one
//       vec3  guideAlbedo;     // demodulation albedo for reconstruction
//       float guideRoughness;  // representative roughness for reconstruction
//   };

ClosureData makeClosureData(int closureType, vec3 L, vec3 V, vec3 N, vec3 P, float occlusion)
{
    return $closureDataConstructor;
}

// Convenience constructor for the path-tracing queries. Kept separate so the
// stock six-argument signature keeps working for unmodified upstream code.
ClosureData mx_pt_closure_data(int closureType, vec3 L, vec3 V, vec3 N, vec3 P,
                               vec4 wavelengths, vec3 u)
{
    ClosureData cd = makeClosureData(closureType, L, V, N, P, 1.0);
    cd.wavelengths = wavelengths;
    cd.u = u;
    cd.lobePdf = 1.0;
    return cd;
}
