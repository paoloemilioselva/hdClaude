// Numerical validation of the genglsl_pt closures, on the GPU.
//
// This is the phase 4 acceptance gate from docs/materialx-codegen.md 8. Every
// preceding test asked whether the generated code *compiles*; these ask whether
// it is *right*.
//
// Two properties per closure, measured by brute force:
//
//   Energy.   Sampling a direction and evaluating f and pdf at it gives a
//             weight f/pdf whose mean is the directional albedo. A physical
//             closure cannot return more energy than it receives, so that mean
//             must not exceed one. This catches a wrong normalisation, a
//             missing Jacobian, or a sampler and density that disagree by a
//             constant factor.
//
//   Density.  Integrating the reported pdf over the sphere must give one.
//             This is the test that catches a combinator reporting a selected
//             child's density instead of the mixture -- an error the energy
//             test cannot see, because f/pdf stays self-consistent and only
//             the *distribution* is wrong.
//
// Requires a GPU: the closures are GLSL, and executing them is the point.

#include "test_support.h"

#include "hdclaude/gpu/compute_pipeline.h"
#include "hdclaude/gpu/glsl_compiler.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"
#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXCore/Document.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mx = MaterialX;
using namespace hdclaude;

namespace {

constexpr std::uint32_t kSampleCount = 1048576;
constexpr float kFixedScale = 512.0f;  // must match the kernel

struct PushParams {
    std::uint32_t sampleCount;
    std::uint32_t seed;
    float viewTheta;
    std::uint32_t closureType;
};

// Must match mtlx/pbrlib/genglsl_pt/lib/mx_closure_type.glsl.
constexpr std::uint32_t kClosureReflection = 1;

std::string ReadFile(const char* path)
{
    std::ifstream file(path);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

mx::NodePtr AddNode(mx::DocumentPtr doc, const std::string& category,
                    const std::string& name, const std::string& type)
{
    mx::NodePtr node = doc->addNode(category, name, type);
    if (!node || !node->getNodeDef()) {
        std::fprintf(stderr, "  no nodedef for '%s'\n", category.c_str());
        return nullptr;
    }
    return node;
}

template <typename T>
void SetValue(mx::NodePtr node, const std::string& input, const T& value)
{
    if (!node) return;
    if (mx::InputPtr port = node->addInputFromNodeDef(input)) {
        port->setValue(value);
    }
}

void Connect(mx::NodePtr node, const std::string& input, mx::NodePtr source)
{
    if (!node || !source) return;
    if (mx::InputPtr port = node->addInputFromNodeDef(input)) {
        port->setConnectedNode(source);
    }
}

/// Wrap a single BSDF node in a surface and a material.
mx::DocumentPtr WrapInMaterial(mx::DocumentPtr doc, mx::NodePtr bsdf)
{
    mx::NodePtr surface = AddNode(doc, "surface", "vSurface", "surfaceshader");
    Connect(surface, "bsdf", bsdf);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "vMaterial", "material");
    Connect(material, "surfaceshader", surface);
    return doc;
}

struct Measurement {
    bool ok = false;
    double albedo = 0.0;
    double densityIntegral = 0.0;
    /// Fraction of sampled directions the closure reports zero density for.
    /// Not an error: a visible-normal sampler can reflect about a microfacet
    /// tilted far enough that the result lies below the horizon, and the
    /// closure cannot scatter there. Those samples carry weight zero and the
    /// estimator stays unbiased -- but the probability mass they hold is
    /// exactly what the reported density integral is missing.
    double discardedFraction = 0.0;
    std::uint32_t sampledCount = 0;
    std::uint32_t nonFinite = 0;
    std::uint32_t zeroPdf = 0;
    std::uint32_t negative = 0;
    /// True if a fixed-point accumulator came close enough to wrapping that
    /// the result cannot be trusted. A wrapped sum produces a plausible number,
    /// not an obviously broken one, so this is detected rather than assumed
    /// away.
    bool saturated = false;
    std::string error;
};

class Validator {
  public:
    Validator(const VulkanContext& context, VulkanAllocator& allocator,
              const GlslCompiler& compiler)
        : _context(context), _allocator(allocator), _compiler(compiler)
    {
        _libraries = LoadDefaultMaterialXLibraries();
        _kernel = ReadFile(HDCLAUDE_VALIDATION_KERNEL);
    }

    bool Ready() const { return !_kernel.empty() && _libraries != nullptr; }

    Measurement Measure(mx::DocumentPtr doc, const std::string& name,
                        float viewTheta)
    {
        Measurement result;

        // --- Generate -------------------------------------------------------
        std::string source;
        try {
            mx::ShaderGeneratorPtr generator = PathTracerShaderGenerator::create();
            mx::GenContext genContext(generator);
            genContext.registerSourceCodeSearchPath(DefaultMaterialXSourceSearchPath());
            // Reduced interface: node values set directly in this test become
            // compile-time constants rather than members of the material's
            // public uniform block. The renderer wants the complete interface,
            // because it drives those parameters; a closure unit test wants the
            // values baked in, so the measurement cannot be wrong because a
            // uniform upload was.
            genContext.getOptions().shaderInterfaceType = mx::SHADER_INTERFACE_REDUCED;

            std::vector<mx::TypedElementPtr> renderable;
            mx::findRenderableElements(doc, renderable);
            if (renderable.empty()) {
                result.error = "no renderable element";
                return result;
            }
            mx::ShaderPtr shader =
                generator->generate(name, renderable.front(), genContext);
            source = shader->getSourceCode(mx::Stage::PIXEL);
        } catch (const std::exception& error) {
            result.error = std::string("generation failed: ") + error.what();
            return result;
        }

        // --- Compile --------------------------------------------------------
        GlslCompileOptions options;
        options.moduleName = name;
        const GlslCompileResult compiled =
            _compiler.Compile(source + _kernel, options);
        if (!compiled.ok) {
            result.error = "compilation failed:\n" + compiled.log;
            return result;
        }

        // --- Run ------------------------------------------------------------
        BufferDescription description;
        description.size = 8 * sizeof(std::uint32_t);
        description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        description.domain = BufferDomain::HostReadback;
        description.debugName = "validation.results";
        VulkanBuffer results(_allocator, description);
        std::memset(results.MappedData(), 0, static_cast<std::size_t>(description.size));

        // MaterialX still emits a public uniform block for the surface and
        // displacement shader interface even with a reduced interface, so the
        // binding has to exist. It is zero-filled: with a reduced interface
        // every value this test cares about is a constant in the code.
        BufferDescription uniformDescription;
        uniformDescription.size = 4096;
        uniformDescription.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uniformDescription.domain = BufferDomain::HostUpload;
        uniformDescription.debugName = "validation.materialUniforms";
        VulkanBuffer materialUniforms(_allocator, uniformDescription);
        std::memset(materialUniforms.MappedData(), 0,
                    static_cast<std::size_t>(uniformDescription.size));

        std::vector<BindingDescription> bindings(2);
        bindings[0].binding = 0;
        bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].debugName = "results";
        bindings[1].binding = 1;
        bindings[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].debugName = "materialUniforms";

        ComputePipeline pipeline(_context, compiled.spirv, bindings,
                                 sizeof(PushParams), name);
        VkDescriptorSet set = pipeline.AllocateSet();
        pipeline.WriteBuffer(set, 0, results);
        pipeline.WriteBuffer(set, 1, materialUniforms,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

        PushParams push{kSampleCount, 0x9E3779B9u, viewTheta, kClosureReflection};
        const std::uint32_t groups = (kSampleCount + 63) / 64;

        _context.SubmitImmediate([&](VkCommandBuffer command) {
            pipeline.Dispatch(command, set, groups, 1, 1, &push, sizeof(push));
        });

        const auto* sums = static_cast<const std::uint32_t*>(results.MappedData());

        // Saturation guard. The accumulators are 32-bit fixed point, and the
        // safe bound depends on the scale, the sample count, and the values a
        // closure returns -- three things that drift independently. Anything
        // above three quarters of the range is treated as untrustworthy.
        constexpr std::uint32_t kSaturationLimit = 0xC0000000u;
        for (int i : {0, 2}) {
            if (sums[i] >= kSaturationLimit) {
                result.saturated = true;
            }
        }
        result.sampledCount = sums[1];
        result.nonFinite = sums[4];
        result.zeroPdf = sums[5];
        result.negative = sums[6];
        if (sums[1] > 0) {
            result.albedo = (sums[0] / kFixedScale) / static_cast<double>(sums[1]);
        }
        if (sums[3] > 0) {
            result.densityIntegral =
                (sums[2] / kFixedScale) / static_cast<double>(sums[3]);
        }
        if (sums[1] > 0) {
            result.discardedFraction =
                static_cast<double>(sums[5]) / static_cast<double>(sums[1]);
        }
        result.ok = true;
        return result;
    }

    mx::DocumentPtr NewDocument()
    {
        mx::DocumentPtr doc = mx::createDocument();
        doc->importLibrary(_libraries);
        return doc;
    }

  private:
    const VulkanContext& _context;
    VulkanAllocator& _allocator;
    const GlslCompiler& _compiler;
    mx::DocumentPtr _libraries;
    std::string _kernel;
};

void Report(const char* label, const Measurement& m)
{
    std::printf("  %-30s albedo %.4f   density %.4f + discarded %.4f = %.4f\n",
                label, m.albedo, m.densityIntegral, m.discardedFraction,
                m.densityIntegral + m.discardedFraction);
    if (m.nonFinite || m.negative || m.saturated) {
        std::printf("  %-30s   %u non-finite, %u negative%s\n", "", m.nonFinite,
                    m.negative, m.saturated ? ", SATURATED" : "");
    }
}

/// Assert the two universal properties.
///
/// Energy: the mean of f/pdf over sampled directions is the directional albedo,
/// and a physical closure cannot return more energy than it receives.
///
/// Probability mass: the reported density integrated over the sphere, *plus*
/// the probability of sampling a direction the closure reports zero density
/// for, must come to one.
///
/// That second form is the correct invariant and the first version of this test
/// got it wrong. A visible-normal sampler can reflect about a microfacet tilted
/// far enough that the result falls below the horizon, where the closure cannot
/// scatter; those samples are discarded with weight zero and the estimator stays
/// unbiased, but their probability mass is missing from the density integral. At
/// roughness 0.8 that is 37% of samples, so the bare integral comes to 0.62 and
/// looks catastrophic while nothing is wrong. Asserting the sum instead measures
/// what actually has to hold.
///
/// A combinator reporting a selected child's density instead of the mixture
/// still fails this, which is what the test is for.
void CheckClosure(const char* label, const Measurement& m, double tolerance)
{
    if (!m.ok) {
        std::fprintf(stderr, "  %s: %s\n", label, m.error.c_str());
    }
    CHECK(m.ok);
    if (!m.ok) return;

    Report(label, m);

    if (m.saturated) {
        std::fprintf(stderr,
                     "  %s: fixed-point accumulator near overflow; lower "
                     "kFixedScale or the sample count\n",
                     label);
    }
    CHECK(!m.saturated);
    CHECK_EQ(m.nonFinite, std::uint32_t(0));
    CHECK_EQ(m.negative, std::uint32_t(0));
    CHECK(m.sampledCount > kSampleCount / 4);

    // Energy conservation.
    CHECK(m.albedo <= 1.02);

    // Total probability mass.
    CHECK_NEAR(m.densityIntegral + m.discardedFraction, 1.0, tolerance);
}

/// Reflectance of a smooth dielectric interface at one incidence.
///
/// `eta` is the *relative* index -- the transmitted side over the incident one
/// -- so a path inside glass looking out is asked about 1/1.5, not 1.5. The
/// same closed form as `mx_fresnel_dielectric`, written independently here so
/// that agreeing with it means something.
double DielectricFresnel(double cosTheta, double eta)
{
    const double g2 = eta * eta + cosTheta * cosTheta - 1.0;
    if (g2 < 0.0) {
        return 1.0;   // total internal reflection
    }
    const double g = std::sqrt(g2);
    const double a = (g - cosTheta) / (g + cosTheta);
    const double b = ((g + cosTheta) * cosTheta - 1.0) /
                     ((g - cosTheta) * cosTheta + 1.0);
    return 0.5 * a * a * (1.0 + b * b);
}

/// The directional albedo a closure reports, against a closed form.
///
/// The harness evaluates every sampled direction as a *reflection*, so a
/// transmitted sample reports no density, contributes nothing, and is still
/// counted. The mean weight over all samples is therefore the reflection lobe's
/// directional albedo -- which for a smooth interface is exactly its Fresnel
/// reflectance, whichever side the path is on.
///
/// The probability-mass check is deliberately not applied here. It integrates
/// the density by uniform sampling of the sphere, which measures nothing useful
/// for a delta lobe; the rough cases above are what constrain that, and this is
/// what constrains the value.
void CheckReflectance(const char* label, const Measurement& m, double expected,
                      double tolerance)
{
    if (!m.ok) {
        std::fprintf(stderr, "  %s: %s\n", label, m.error.c_str());
    }
    CHECK(m.ok);
    if (!m.ok) return;

    std::printf("  %-30s albedo %.4f (closed form %.4f)\n", label, m.albedo,
                expected);
    CHECK(!m.saturated);
    CHECK_EQ(m.nonFinite, std::uint32_t(0));
    CHECK_EQ(m.negative, std::uint32_t(0));
    CHECK(m.sampledCount > kSampleCount / 4);
    CHECK_NEAR(m.albedo, expected, tolerance);
}

/// Tolerance for the probability-mass check.
///
/// The density integral is estimated by uniform sampling of the sphere, which
/// is a poor estimator for a narrow lobe: at roughness 0.1 a GGX lobe covers
/// about 1% of the sphere, so only 1% of samples carry the whole integral and
/// the variance is large. Broad lobes are measured tightly; narrow ones are
/// given room, and the energy check above -- which importance samples and so
/// does not suffer this -- is what constrains them.
constexpr double kBroadLobeTolerance = 0.02;
constexpr double kNarrowLobeTolerance = 0.10;

}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("hdClaudeClosureValidationTests\n");

    std::unique_ptr<VulkanContext> context;
    try {
        VulkanContextOptions options;
        options.enableValidation = true;
        context = std::make_unique<VulkanContext>(options);
    } catch (const VulkanError& error) {
        std::printf("SKIP: no usable Vulkan device (%s)\n", error.what());
        return 0;
    }
    if (!context->ValidationEnabled()) {
        std::fprintf(stderr,
                     "FAIL: validation layer unavailable; see docs/building.md\n");
        return 1;
    }

    // Core validation alone is not the gate. Synchronisation validation is what
    // reports a buffer one kernel writes that the next cannot yet see, and it
    // can be off with the layer present, so a clean count would say nothing.
    if (!context->SynchronisationValidationEnabled()) {
        std::fprintf(stderr,
                     "FAIL: the validation layer is running without "
                     "synchronisation validation (it lacks "
                     "VK_EXT_layer_settings), so the validation gate would "
                     "pass without checking kernel hazards; see "
                     "docs/building.md\n");
        return 1;
    }

    const GlslCompiler compiler;
    {
        VulkanAllocator allocator(*context);
        Validator validator(*context, allocator, compiler);
        if (!validator.Ready()) {
            std::fprintf(stderr, "FAIL: could not load libraries or kernel\n");
            return 1;
        }

        // --- Diffuse ---------------------------------------------------------
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr n = AddNode(doc, "oren_nayar_diffuse_bsdf", "vD", "BSDF");
            SetValue(n, "weight", 1.0f);
            SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
            SetValue(n, "roughness", 0.0f);
            CheckClosure("oren_nayar (white, smooth)",
                         validator.Measure(WrapInMaterial(doc, n), "vOrenNayar", 0.3f),
                         kBroadLobeTolerance);
        }
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr n = AddNode(doc, "burley_diffuse_bsdf", "vB", "BSDF");
            SetValue(n, "weight", 1.0f);
            SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
            SetValue(n, "roughness", 0.5f);
            CheckClosure("burley_diffuse (white)",
                         validator.Measure(WrapInMaterial(doc, n), "vBurley", 0.3f),
                         kBroadLobeTolerance);
        }

        // --- Specular --------------------------------------------------------
        for (float roughness : {0.1f, 0.4f, 0.8f}) {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr n = AddNode(doc, "conductor_bsdf", "vC", "BSDF");
            SetValue(n, "weight", 1.0f);
            SetValue(n, "roughness", mx::Vector2(roughness, roughness));
            char label[64];
            std::snprintf(label, sizeof(label), "conductor (roughness %.1f)",
                          static_cast<double>(roughness));
            CheckClosure(label,
                         validator.Measure(WrapInMaterial(doc, n), "vConductor", 0.4f),
                         roughness < 0.3f ? kNarrowLobeTolerance : kBroadLobeTolerance);
        }
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr n = AddNode(doc, "dielectric_bsdf", "vDi", "BSDF");
            SetValue(n, "weight", 1.0f);
            SetValue(n, "roughness", mx::Vector2(0.3f, 0.3f));
            CheckClosure("dielectric (reflection)",
                         validator.Measure(WrapInMaterial(doc, n), "vDielectric", 0.4f),
                         kBroadLobeTolerance);
        }

        // --- Grazing incidence, where the fits run out ------------------------
        //
        // Every probe above looks at a surface from somewhere comfortable. The
        // renderer produces non-finite samples once the bounce limit rises
        // above eight, and a long path is mostly made of *awkward* angles: at a
        // silhouette `NdotV` is clamped to an epsilon, and the analytic fits
        // the closures lean on are least trustworthy exactly there.
        //
        // `mx_ggx_energy_compensation` is the specific worry. It returns
        // `1 + Fss * (1 - Ess) / Ess`, dividing by a lobe energy that tends to
        // zero as the view goes edge-on, so if `Ess` underflows the closure
        // returns an infinity and every path through it is poisoned. This asks
        // whether it does.
        //
        // Exactly ninety degrees and beyond are included deliberately. A
        // concave surface presents both: the inside of a ring or a tab is seen
        // edge-on and past edge-on all along its curve, and `NdotV` is then
        // clamped to an epsilon that several of these expressions divide by.
        // That is where the renderer's non-finite samples live -- they sit on
        // the inside of the shader ball's base ring and on the inside of its
        // tab, and nowhere else.
        // Three per cent rather than the two ordinary angles are held to, and
        // the extra one per cent is named rather than hidden. A smooth
        // dielectric seen exactly edge-on from inside reads 1.0219 where the
        // closed form is exactly one, and the excess is `mx_ggx_dir_albedo`'s
        // own accuracy as `NdotV` goes to zero: the compensation term is
        // `1 + Fss (1 - Ess) / Ess`, which under total internal reflection is
        // exactly `1 / Ess`, and multiplying a lobe whose true energy is `Ess`
        // by that returns one only insofar as the fit is right about `Ess`. An
        // upstream fit's error at the far end of its domain, not a transport
        // defect; it goes away with a better `Ess` rather than with anything
        // here. Recorded so this bound is a known quantity and not a round
        // number chosen to pass.
        constexpr double kGrazingEnergyBound = 1.03;
        for (float theta : {1.4f, 1.5f, 1.55f, 1.5707963f, 1.6f, 2.0f}) {
            for (float roughness : {0.1f, 0.4f, 0.8f}) {
                mx::DocumentPtr doc = validator.NewDocument();
                mx::NodePtr c = AddNode(doc, "conductor_bsdf", "vGzC", "BSDF");
                SetValue(c, "weight", 1.0f);
                SetValue(c, "roughness", mx::Vector2(roughness, roughness));
                char conductorLabel[80];
                std::snprintf(conductorLabel, sizeof(conductorLabel),
                              "conductor grazing %.4f r%.1f", theta, roughness);
                const Measurement mc =
                    validator.Measure(WrapInMaterial(doc, c), "vGrazingC", theta);
                CHECK(mc.ok);
                if (mc.ok) {
                    std::printf("  %-34s albedo %.4f  %u non-finite, %u negative\n",
                                conductorLabel, mc.albedo, mc.nonFinite,
                                mc.negative);
                    CHECK_EQ(mc.nonFinite, std::uint32_t(0));
                    CHECK(mc.albedo <= kGrazingEnergyBound);
                }

                doc = validator.NewDocument();
                mx::NodePtr n = AddNode(doc, "dielectric_bsdf", "vGz", "BSDF");
                SetValue(n, "weight", 1.0f);
                SetValue(n, "ior", 1.5f);
                SetValue(n, "roughness", mx::Vector2(roughness, roughness));
                n->setInputValue("scatter_mode", std::string("RT"), "string");

                char label[80];
                std::snprintf(label, sizeof(label),
                              "dielectric grazing %.4f r%.1f", theta, roughness);
                const Measurement m =
                    validator.Measure(WrapInMaterial(doc, n), "vGrazing", theta);
                CHECK(m.ok);
                if (!m.ok) continue;
                std::printf("  %-34s albedo %.4f  %u non-finite, %u negative\n",
                            label, m.albedo, m.nonFinite, m.negative);
                CHECK_EQ(m.nonFinite, std::uint32_t(0));
                CHECK_EQ(m.negative, std::uint32_t(0));
                CHECK(m.albedo <= kGrazingEnergyBound);
            }
        }

        // --- The inside of a dielectric --------------------------------------
        //
        // Every measurement above looks at a surface from outside it, which is
        // the only side a camera ray ever starts on and, until a scattering
        // medium existed, very nearly the only side anything reached. It is
        // also the side on which an interface's two Fresnel curves agree near
        // normal incidence, which is why handing the absolute index to both
        // sides went unnoticed.
        //
        // From inside, the relative index is 1/1.5 and the curve is a different
        // one: it reaches unity at the critical angle, 41.8 degrees, and stays
        // there. The outside curve reaches unity only at grazing. Just below
        // the critical angle the two differ by a factor of two, and past it by
        // a factor of twenty.
        //
        // `viewTheta` past 90 degrees puts the view direction on the far side
        // of the surface. The lobe is RT: only a lobe that can transmit can
        // have carried a path inside itself, so only such a lobe reads the
        // inside curve. A reflection-only lobe hit from behind is a back face
        // in the air, not glass, and keeps the authored index.
        struct InsideProbe { float theta; const char* name; };
        const InsideProbe insideProbes[] = {
            {2.531f, "dielectric inside, 35 deg"},
            {2.897f, "dielectric inside, 14 deg"},
            {2.200f, "dielectric inside, past critical"},
            {1.800f, "dielectric inside, grazing"},
        };
        for (const InsideProbe& probe : insideProbes) {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr n = AddNode(doc, "dielectric_bsdf", "vDiIn", "BSDF");
            SetValue(n, "weight", 1.0f);
            SetValue(n, "ior", 1.5f);
            SetValue(n, "roughness", mx::Vector2(0.0f, 0.0f));
            n->setInputValue("scatter_mode", std::string("RT"), "string");

            // The incidence the harness will present, and the reflectance the
            // closed form gives for it on the inside.
            const double cosTheta = std::abs(std::cos(double(probe.theta)));
            const double expected = DielectricFresnel(cosTheta, 1.0 / 1.5);

            CheckReflectance(probe.name,
                             validator.Measure(WrapInMaterial(doc, n), "vDiIn",
                                               probe.theta),
                             expected, 0.02);
        }
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr n = AddNode(doc, "sheen_bsdf", "vS", "BSDF");
            SetValue(n, "weight", 1.0f);
            SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
            SetValue(n, "roughness", 0.3f);
            CheckClosure("sheen",
                         validator.Measure(WrapInMaterial(doc, n), "vSheen", 0.3f),
                         kBroadLobeTolerance);
        }

        // --- Combinators -----------------------------------------------------
        // The reason the density test exists. A `mix` that reported the
        // selected child's density instead of the mixture would pass every
        // energy check above and fail here.
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr a = AddNode(doc, "oren_nayar_diffuse_bsdf", "vMa", "BSDF");
            SetValue(a, "weight", 1.0f);
            SetValue(a, "color", mx::Color3(1.0f, 1.0f, 1.0f));
            mx::NodePtr b = AddNode(doc, "conductor_bsdf", "vMb", "BSDF");
            SetValue(b, "weight", 1.0f);
            SetValue(b, "roughness", mx::Vector2(0.4f, 0.4f));
            mx::NodePtr m = AddNode(doc, "mix", "vMix", "BSDF");
            Connect(m, "fg", b);
            Connect(m, "bg", a);
            SetValue(m, "mix", 0.5f);
            CheckClosure("mix(conductor, diffuse)",
                         validator.Measure(WrapInMaterial(doc, m), "vMix", 0.3f),
                         kBroadLobeTolerance);
        }
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr base = AddNode(doc, "oren_nayar_diffuse_bsdf", "vLb", "BSDF");
            SetValue(base, "weight", 1.0f);
            SetValue(base, "color", mx::Color3(1.0f, 1.0f, 1.0f));
            mx::NodePtr top = AddNode(doc, "dielectric_bsdf", "vLt", "BSDF");
            SetValue(top, "weight", 1.0f);
            SetValue(top, "roughness", mx::Vector2(0.2f, 0.2f));
            mx::NodePtr l = AddNode(doc, "layer", "vLayer", "BSDF");
            Connect(l, "top", top);
            Connect(l, "base", base);
            CheckClosure("layer(dielectric, diffuse)",
                         validator.Measure(WrapInMaterial(doc, l), "vLayer", 0.3f),
                         kBroadLobeTolerance);
        }
        {
            mx::DocumentPtr doc = validator.NewDocument();
            mx::NodePtr a = AddNode(doc, "oren_nayar_diffuse_bsdf", "vAa", "BSDF");
            SetValue(a, "weight", 1.0f);
            SetValue(a, "color", mx::Color3(0.5f, 0.5f, 0.5f));
            mx::NodePtr b = AddNode(doc, "conductor_bsdf", "vAb", "BSDF");
            SetValue(b, "weight", 0.5f);
            SetValue(b, "roughness", mx::Vector2(0.5f, 0.5f));
            mx::NodePtr s = AddNode(doc, "add", "vAdd", "BSDF");
            Connect(s, "in1", a);
            Connect(s, "in2", b);
            CheckClosure("add(diffuse, conductor)",
                         validator.Measure(WrapInMaterial(doc, s), "vAdd", 0.3f),
                         kBroadLobeTolerance);
        }

        const std::uint64_t errors = context->ValidationErrorCount();
        if (errors != 0) {
            std::fprintf(stderr, "FAIL: %llu validation error(s). Last: %s\n",
                         static_cast<unsigned long long>(errors),
                         context->LastValidationError().c_str());
        }
        CHECK_EQ(errors, std::uint64_t(0));
    }

    return hdclaude_test::Summarize("hdClaudeClosureValidationTests");
}
