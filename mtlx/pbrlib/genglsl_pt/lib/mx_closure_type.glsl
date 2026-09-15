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
vec3 hdclaude_sample_u = vec3(0.0);     // sample: xy direction, z seeds lobe choices
uint hdclaude_selection_count = 0u;     // lobe choices made so far; see mx_pt_selection_random

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

// Interior media are published on the BSDF struct, not in a global.
//
// Volumetric absorption and scattering are integrated *along a ray inside the
// medium*, not evaluated at a surface, so a closure's job is to record the
// parameters and the integrator's job is to transport with them. Two closures
// record them: `anisotropic_vdf`, for the interior a transmissive surface
// encloses, and `subsurface_bsdf`, because a random walk beneath a surface and
// one inside a volume are the same walk.
//
// They travel on the struct rather than in a global because a global cannot
// answer the question the integrator actually asks. Both `standard_surface` and
// `open_pbr_surface` instantiate `subsurface_bsdf` *and* `anisotropic_vdf`
// unconditionally, gated downstream by a `mix` on the authored weight, so a
// material with no subsurface and no transmission still runs both closures and
// still publishes both media. A global records whichever ran last. The path
// enters the medium belonging to the lobe that carried it through the
// interface, and the combinator that chose that lobe is the only thing that
// knows which one it was -- so the medium is propagated by the same selection
// that propagates `sampledL`, and read from the sampling pass.
//
// Under CLOSURE_TYPE_PT_SAMPLE the fields name the interior of the lobe that
// was sampled. Under the evaluation types they name one chosen at random as
// the sampling pass would have chosen it for the direction evaluated; see
// hdclaude_select_medium.

// How the second vector of a medium is to be read. The two closures that
// publish an interior describe it in different terms, and which terms decides
// what the traversal kernel does with it -- so the parameterisation travels
// with the medium rather than being assumed.
#define HDCLAUDE_MEDIUM_NONE 0.0
#define HDCLAUDE_MEDIUM_ALBEDO 1.0
#define HDCLAUDE_MEDIUM_REFLECTANCE 2.0

/// Record an interior medium whose colour is a pair of coefficients.
///
/// What `anisotropic_vdf` authors, and what OpenPBR derives from
/// `transmission_color` and `transmission_depth`. The two are stored as an
/// extinction and the ratio between them, because that ratio -- the
/// single-scattering albedo -- is bounded in [0, 1] and survives being resolved
/// to wavelengths, where two coefficients resolved separately do not keep the
/// ratio between them at all.
void hdclaude_publish_medium(inout BSDF bsdf, vec3 absorption, vec3 scattering,
                             float anisotropy)
{
    vec3 a = max(absorption, vec3(0.0));
    vec3 s = max(scattering, vec3(0.0));
    vec3 extinction = a + s;
    bsdf.mediumExtinction = extinction;
    bsdf.mediumAlbedo = vec3(extinction.x > 0.0 ? s.x / extinction.x : 0.0,
                             extinction.y > 0.0 ? s.y / extinction.y : 0.0,
                             extinction.z > 0.0 ? s.z / extinction.z : 0.0);
    bsdf.mediumAnisotropy = clamp(anisotropy, -0.99, 0.99);
    bsdf.mediumKind = HDCLAUDE_MEDIUM_ALBEDO;
}

/// Record an interior medium whose colour is the light that comes back out.
///
/// What `subsurface_bsdf` authors. MaterialX documents its `color` as the
/// diffuse reflectivity and OpenPBR as "the observed reflection color", so it
/// is a reflectance and not a coefficient ratio, and the relation between the
/// two -- van de Hulst's, which OpenPBR states in closed form -- is steeply
/// nonlinear: a reflectance of 0.6 needs collisions that survive 95 per cent of
/// the time.
///
/// The reflectance is passed through *unconverted*, and the integrator inverts
/// it per wavelength. Inverting here, per RGB channel, would be the same error
/// as any other nonlinearity applied before a spectrum is resolved: the walk
/// would return a spectrum whose projection is not the authored colour, and a
/// magenta subsurface material measured a seventh short in red for exactly that
/// reason. A reflectance is also the one thing the upsampling fit is built for,
/// so this is the form that survives the journey.
void hdclaude_publish_subsurface(inout BSDF bsdf, vec3 extinction,
                                 vec3 reflectance, float anisotropy)
{
    bsdf.mediumExtinction = max(extinction, vec3(0.0));
    bsdf.mediumAlbedo = clamp(reflectance, vec3(0.0), vec3(1.0));
    bsdf.mediumAnisotropy = clamp(anisotropy, -0.99, 0.99);
    bsdf.mediumKind = HDCLAUDE_MEDIUM_REFLECTANCE;
}

/// Copy an interior medium from the lobe a combinator selected.
void hdclaude_carry_medium(inout BSDF result, BSDF selected)
{
    result.mediumExtinction = selected.mediumExtinction;
    result.mediumAlbedo = selected.mediumAlbedo;
    result.mediumAnisotropy = selected.mediumAnisotropy;
    result.mediumKind = selected.mediumKind;
}

/// No interior. Written by the evaluation types, where nothing has been
/// selected and so no medium is meaningful, rather than left alone: a
/// combinator's `out BSDF` starts uninitialised, and leaving these fields as
/// whatever the generated code declared is how
/// `layer(dielectric_bsdf, anisotropic_vdf)` once rendered black.
void hdclaude_clear_medium(inout BSDF result)
{
    result.mediumExtinction = vec3(0.0);
    result.mediumAlbedo = vec3(0.0);
    result.mediumAnisotropy = 0.0;
    result.mediumKind = HDCLAUDE_MEDIUM_NONE;
}

// Which interior a *shadow ray* crosses, under evaluation.
//
// Next-event estimation through a transmissive surface asks the evaluation
// types about a light behind it. When that light is inside the object the
// shadow ray travels through the object's interior and has to be attenuated by
// it, so the evaluation has to say which interior that is -- and nothing was
// selected, which is why these types used to clear the medium.
//
// The answer has to be the one the *other* strategy gives, or the two cannot be
// weighed against each other. A scattered path enters the interior of the lobe
// the sampling pass chose, and given that it left along `L`, the chance that
// lobe `i` was the one is
//
//     P_i * pdf_i(L) / pdf(L)
//
// with `P_i` the product of the combinators' selection probabilities on the way
// to it. So an evaluation reaches the same distribution by choosing, at every
// combinator, between its two children in proportion to each child's selection
// probability times its own density at `L`: the densities telescope, since a
// child's density is already the mixture of its leaves'. A lobe with no
// interior is chosen as often as it would carry the path, and choosing it
// clears the medium, which attenuates by nothing. The expected transmittance is
// then exactly the one the scattered path's estimate has at `L`, and next-event
// estimation multiplies the whole response by the chosen interior's.
//
// `u` is a fresh uniform, and `weightA`, `weightB` the two children's selection
// probabilities times their densities.
void hdclaude_select_medium(inout BSDF result, BSDF a, float weightA, BSDF b,
                            float weightB, float u)
{
    float total = max(weightA, 0.0) + max(weightB, 0.0);
    if (!(total > 0.0))
    {
        hdclaude_clear_medium(result);
        return;
    }
    if (u * total < max(weightA, 0.0))
    {
        hdclaude_carry_medium(result, a);
    }
    else
    {
        hdclaude_carry_medium(result, b);
    }
}

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
//       vec3  guideDiffuse;    // diffuse albedo, for demodulation
//       vec3  guideSpecular;   // specular albedo: reflectivity for this view
//       vec3  guideNormal;     // shading normal the lobes answer to
//       float guideRoughness;  // GGX alpha; a guide buffer takes sqrt(alpha)
//       vec3  mediumExtinction;  // interior sigma_t, of the selected lobe
//       vec3  mediumAlbedo;      // its albedo, read per mediumKind
//       float mediumAnisotropy;  // interior Henyey-Greenstein g
//       float mediumKind;        // none, single-scattering albedo, reflectance
//   };
//
// The struct definition and its default-value expression come from the same
// Syntax registration, so they cannot drift apart. `isDelta` is a float so the
// default stays a plain aggregate literal.
