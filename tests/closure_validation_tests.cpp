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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
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
// Not a MaterialX closure type: the harness's request to evaluate each direction
// the way `shade` does, REFLECTION in front of the normal and TRANSMISSION
// behind it.
constexpr std::uint32_t kClosureBySide = 0;

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

constexpr double kPi = 3.14159265358979323846;

// The chi-squared test's resolution: cells over the sphere, and quadrature
// points per side inside each. Theta is split evenly so that the horizon lies
// exactly on a cell boundary -- a reflection lobe's density jumps to zero there,
// and a cell straddling a discontinuity is integrated worst by a regular grid.
constexpr std::uint32_t kChiThetaBins = 20;
constexpr std::uint32_t kChiPhiBins = 40;
constexpr std::uint32_t kChiSubdivisions = 24;

struct ChiSquareParams {
    std::uint32_t count;
    std::uint32_t seed;
    float viewTheta;
    std::uint32_t mode;
    std::uint32_t thetaBins;
    std::uint32_t phiBins;
    std::uint32_t subdivisions;
    std::uint32_t reserved;
};

struct ChiSquareHistograms {
    bool ok = false;
    std::vector<double> observed;
    std::vector<double> expected;
    std::uint32_t kept = 0;
    /// The directional albedo two ways: the importance-sampled mean of f / pdf
    /// over every sample, with its standard error, and the response integrated
    /// by the same quadrature as the density.
    double sampledAlbedo = 0.0;
    double sampledAlbedoError = 0.0;
    double integratedAlbedo = 0.0;
    /// The reconstruction guides as the sampling pass and an evaluation pass
    /// publish them: diffuse, specular, normal, roughness, ten values each.
    std::array<double, 20> guides{};
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
        _chiSquareKernel = ReadFile(HDCLAUDE_CHI2_KERNEL);
    }

    bool Ready() const
    {
        return !_kernel.empty() && !_chiSquareKernel.empty() &&
               _libraries != nullptr;
    }

    /// Where a closure's sampled directions land, and where its reported
    /// density says they should.
    ///
    /// Both are histograms over the same (theta, phi) cells of the whole
    /// sphere. `observed` counts the samples `shade` would keep -- a finite,
    /// positive density at the direction chosen -- and `expected` is the
    /// number of all `kSampleCount` samples the density puts in each cell. On
    /// the directions a sampler keeps, its distribution has to *be* that
    /// density, so the two agree in every cell, not only in total.
    ChiSquareHistograms MeasureDistribution(mx::DocumentPtr doc,
                                            const std::string& name,
                                            float viewTheta,
                                            std::uint32_t furnaceBatches = 1)
    {
        ChiSquareHistograms result;

        GlslCompileResult compiled;
        if (!Build(doc, name, _chiSquareKernel, compiled, result.error)) {
            return result;
        }

        const std::uint32_t cells = kChiThetaBins * kChiPhiBins;
        const std::uint32_t densityPoints =
            cells * kChiSubdivisions * kChiSubdivisions;
        const std::uint32_t sampleSlots = kSampleCount * 5;
        const std::uint32_t densitySlots = densityPoints * 2;

        BufferDescription description;
        description.size =
            std::uint64_t(std::max(sampleSlots, densitySlots)) * sizeof(float);
        description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        description.domain = BufferDomain::HostReadback;
        description.debugName = "chi2.values";
        VulkanBuffer values(_allocator, description);

        BufferDescription uniformDescription;
        uniformDescription.size = 4096;
        uniformDescription.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uniformDescription.domain = BufferDomain::HostUpload;
        uniformDescription.debugName = "chi2.materialUniforms";
        VulkanBuffer materialUniforms(_allocator, uniformDescription);
        std::memset(materialUniforms.MappedData(), 0,
                    static_cast<std::size_t>(uniformDescription.size));

        std::vector<BindingDescription> bindings(2);
        bindings[0].binding = 0;
        bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].debugName = "values";
        bindings[1].binding = 1;
        bindings[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].debugName = "materialUniforms";

        ComputePipeline pipeline(_context, compiled.spirv, bindings,
                                 sizeof(ChiSquareParams), name + ".chi2");
        VkDescriptorSet set = pipeline.AllocateSet();
        pipeline.WriteBuffer(set, 0, values);
        pipeline.WriteBuffer(set, 1, materialUniforms,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

        const auto run = [&](std::uint32_t mode, std::uint32_t count,
                             std::uint32_t seed) {
            std::memset(values.MappedData(), 0,
                        static_cast<std::size_t>(description.size));
            ChiSquareParams push{count,         seed,          viewTheta,
                                 mode,          kChiThetaBins, kChiPhiBins,
                                 kChiSubdivisions, 0u};
            const std::uint32_t groups = (count + 63) / 64;
            _context.SubmitImmediate([&](VkCommandBuffer command) {
                pipeline.Dispatch(command, set, groups, 1, 1, &push,
                                  sizeof(push));
            });
            return static_cast<const float*>(values.MappedData());
        };

        const double thetaStep = kPi / kChiThetaBins;
        const double phiStep = 2.0 * kPi / kChiPhiBins;

        // --- Observed ---------------------------------------------------------
        result.observed.assign(cells, 0.0);
        {
            // Batch 0 is the chi-squared sample and every batch feeds the
            // furnace, which a low-albedo lobe needs more of than the
            // histogram does to resolve half a per cent.
            double weightSum = 0.0;
            double weightSquares = 0.0;
            for (std::uint32_t batch = 0; batch < furnaceBatches; ++batch) {
                const float* slots =
                    run(0, kSampleCount, 0x2545F491u + batch * 0x9E3779B9u);
                // Every sample counts toward the furnace's mean, discarded ones
                // as zero, which is what they contribute to a render.
                for (std::uint32_t i = 0; i < kSampleCount; ++i) {
                    const double weight = double(slots[std::size_t(i) * 5 + 4]);
                    weightSum += weight;
                    weightSquares += weight * weight;
                }
                if (batch != 0) {
                    continue;
                }
                for (std::uint32_t i = 0; i < kSampleCount; ++i) {
                    const float* s = slots + std::size_t(i) * 5;
                    if (s[3] == 0.0f) {
                        continue;
                    }
                    const double z = std::clamp(double(s[2]), -1.0, 1.0);
                    const double theta = std::acos(z);
                    double phi = std::atan2(double(s[1]), double(s[0]));
                    if (phi < 0.0) {
                        phi += 2.0 * kPi;
                    }
                    const std::uint32_t t = std::min(
                        std::uint32_t(theta / thetaStep), kChiThetaBins - 1);
                    const std::uint32_t p =
                        std::min(std::uint32_t(phi / phiStep), kChiPhiBins - 1);
                    result.observed[t * kChiPhiBins + p] += 1.0;
                    ++result.kept;
                }
            }
            const double n = double(kSampleCount) * furnaceBatches;
            result.sampledAlbedo = weightSum / n;
            result.sampledAlbedoError = std::sqrt(
                std::max(0.0, weightSquares / n -
                                  result.sampledAlbedo * result.sampledAlbedo) /
                n);
        }

        // --- Expected ---------------------------------------------------------
        result.expected.assign(cells, 0.0);
        {
            const float* slots = run(1, densityPoints, 0u);
            const std::uint32_t perCell = kChiSubdivisions * kChiSubdivisions;
            const double pointArea = thetaStep * phiStep / perCell;
            double albedo = 0.0;
            for (std::uint32_t c = 0; c < cells; ++c) {
                double integral = 0.0;
                for (std::uint32_t k = 0; k < perCell; ++k) {
                    const std::size_t point = std::size_t(c) * perCell + k;
                    integral += double(slots[2 * point]);
                    albedo += double(slots[2 * point + 1]);
                }
                result.expected[c] = integral * pointArea * kSampleCount;
            }
            result.integratedAlbedo = albedo * pointArea;
        }

        {
            const float* slots = run(2, 1, 0u);
            for (std::size_t k = 0; k < result.guides.size(); ++k) {
                result.guides[k] = double(slots[k]);
            }
        }

        result.ok = true;
        return result;
    }

    Measurement Measure(mx::DocumentPtr doc, const std::string& name,
                        float viewTheta,
                        std::uint32_t closureType = kClosureReflection)
    {
        Measurement result;

        GlslCompileResult compiled;
        if (!Build(doc, name, _kernel, compiled, result.error)) {
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

        PushParams push{kSampleCount, 0x9E3779B9u, viewTheta, closureType};
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
    /// Generate `doc` for the path-tracing target and compile it with a
    /// harness kernel appended.
    bool Build(mx::DocumentPtr doc, const std::string& name,
               const std::string& kernel, GlslCompileResult& compiled,
               std::string& error)
    {
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
                error = "no renderable element";
                return false;
            }
            mx::ShaderPtr shader =
                generator->generate(name, renderable.front(), genContext);
            source = shader->getSourceCode(mx::Stage::PIXEL);
        } catch (const std::exception& exception) {
            error = std::string("generation failed: ") + exception.what();
            return false;
        }

        GlslCompileOptions options;
        options.moduleName = name;
        compiled = _compiler.Compile(source + kernel, options);
        if (!compiled.ok) {
            error = "compilation failed:\n" + compiled.log;
            return false;
        }
        return true;
    }

    const VulkanContext& _context;
    VulkanAllocator& _allocator;
    const GlslCompiler& _compiler;
    mx::DocumentPtr _libraries;
    std::string _kernel;
    std::string _chiSquareKernel;
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

    // Total probability mass, unless the caller has it measured elsewhere.
    if (tolerance >= 0.0) {
        CHECK_NEAR(m.densityIntegral + m.discardedFraction, 1.0, tolerance);
    }
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
/// No mass check from the uniform-sphere estimate. For a refracted lobe, whose
/// peak density times 4 pi exceeds the kernel's per-sample clamp, that estimate
/// reads low for a reason that has nothing to do with the closure; the
/// chi-squared quadrature measures the same mass without the clamp and is where
/// those closures are held to it.
constexpr double kEnergyOnly = -1.0;

/// Q(a, x), the regularized upper incomplete gamma function.
///
/// The chi-squared survival function is Q(dof / 2, statistic / 2). A series
/// below x = a + 1 and a continued fraction above it, each where it converges
/// quickly (Numerical Recipes 6.2). Checked against published critical values
/// before anything relies on it.
double RegularizedGammaQ(double a, double x)
{
    if (x <= 0.0) {
        return 1.0;
    }
    const double logPrefactor = -x + a * std::log(x) - std::lgamma(a);
    if (x < a + 1.0) {
        double term = 1.0 / a;
        double sum = term;
        double ap = a;
        for (int n = 0; n < 100000; ++n) {
            ap += 1.0;
            term *= x / ap;
            sum += term;
            if (std::abs(term) < std::abs(sum) * 1.0e-15) {
                break;
            }
        }
        return 1.0 - sum * std::exp(logPrefactor);
    }
    constexpr double kTiny = 1.0e-300;
    double b = x + 1.0 - a;
    double c = 1.0 / kTiny;
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i < 100000; ++i) {
        const double an = -double(i) * (double(i) - a);
        b += 2.0;
        d = an * d + b;
        if (std::abs(d) < kTiny) d = kTiny;
        c = b + an / c;
        if (std::abs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        const double delta = d * c;
        h *= delta;
        if (std::abs(delta - 1.0) < 1.0e-15) {
            break;
        }
    }
    return std::exp(logPrefactor) * h;
}

struct ChiSquareResult {
    double statistic = 0.0;
    int degreesOfFreedom = 0;
    double pValue = 0.0;
    /// Samples landed in a cell the density gives no mass at all. No
    /// statistic is needed to call that wrong.
    double impossibleSamples = 0.0;
    std::size_t worstCell = 0;
    double worstObserved = 0.0;
    double worstExpected = 0.0;
};

/// Pearson's test of observed against expected cell counts.
///
/// Cells expected to hold fewer than five samples are pooled, smallest first,
/// until each pool expects at least five: the statistic is only chi-squared
/// distributed when no cell's expectation is small, which is the same pooling
/// Mitsuba's `ChiSquareTest` does and for the same reason.
ChiSquareResult PearsonChiSquare(const std::vector<double>& observed,
                                 const std::vector<double>& expected)
{
    constexpr double kMinimumExpected = 5.0;
    ChiSquareResult result;

    std::vector<std::size_t> order(expected.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t l, std::size_t r) {
        return expected[l] < expected[r];
    });

    double worstContribution = -1.0;
    int cells = 0;
    double pooledObserved = 0.0;
    double pooledExpected = 0.0;
    const auto add = [&](double o, double e, std::size_t cell) {
        const double contribution = (o - e) * (o - e) / e;
        result.statistic += contribution;
        ++cells;
        if (contribution > worstContribution) {
            worstContribution = contribution;
            result.worstCell = cell;
            result.worstObserved = o;
            result.worstExpected = e;
        }
    };
    for (std::size_t cell : order) {
        if (expected[cell] <= 0.0) {
            result.impossibleSamples += observed[cell];
            continue;
        }
        if (expected[cell] < kMinimumExpected) {
            pooledObserved += observed[cell];
            pooledExpected += expected[cell];
            if (pooledExpected >= kMinimumExpected) {
                add(pooledObserved, pooledExpected, cell);
                pooledObserved = 0.0;
                pooledExpected = 0.0;
            }
            continue;
        }
        add(observed[cell], expected[cell], cell);
    }
    if (pooledExpected > 0.0) {
        add(pooledObserved, pooledExpected, order.front());
    }

    result.degreesOfFreedom = std::max(1, cells - 1);
    result.pValue = RegularizedGammaQ(0.5 * result.degreesOfFreedom,
                                      0.5 * result.statistic);
    return result;
}

/// Assert that a closure samples the density it reports.
///
/// `significance` is the per-test level. With a fixed seed every run is the
/// same run, so this is not flaky in the ordinary sense; but a correct
/// closure still fails with that probability on the one seed chosen, which is
/// why the level is corrected for the number of closures tested.
void CheckDistribution(const char* label, const ChiSquareHistograms& h,
                       double significance)
{
    if (!h.ok) {
        std::fprintf(stderr, "  %s: %s\n", label, h.error.c_str());
    }
    CHECK(h.ok);
    if (!h.ok) return;

    const ChiSquareResult chi = PearsonChiSquare(h.observed, h.expected);
    double totalExpected = 0.0;
    for (double e : h.expected) totalExpected += e;

    const double cellTheta =
        (double(chi.worstCell / kChiPhiBins) + 0.5) * 180.0 / kChiThetaBins;
    const double cellPhi =
        (double(chi.worstCell % kChiPhiBins) + 0.5) * 360.0 / kChiPhiBins;
    std::printf("  %-30s chi2 %9.1f  dof %4d  p %.4f  kept %u  expected %.0f"
                "  worst cell (%.0f, %.0f deg) %.0f vs %.1f\n",
                label, chi.statistic, chi.degreesOfFreedom, chi.pValue, h.kept,
                totalExpected, cellTheta, cellPhi, chi.worstObserved,
                chi.worstExpected);
    if (chi.impossibleSamples > 0.0) {
        std::printf("  %-30s   %.0f samples where the density is zero\n", "",
                    chi.impossibleSamples);
    }
    CHECK_EQ(chi.impossibleSamples, 0.0);
    CHECK(chi.pValue > significance);

    // Not vacuous. A closure whose every sample is discarded has an empty
    // histogram and an empty expectation, and agrees with itself perfectly;
    // `translucent_bsdf` did exactly that, and so a closure has to keep most
    // of what it samples.
    CHECK(h.kept > kSampleCount / 2);

    // Probability mass, from the quadrature rather than from a uniform-sphere
    // estimate. On the directions kept the density integrates to the kept
    // fraction, and the discarded rest is the remainder, so the two totals are
    // the same number of samples. The uniform estimate clamps every value it
    // accumulates to protect a fixed-point sum, which a refracted lobe's peak
    // exceeds; the quadrature has no such limit. One per cent is the
    // quadrature's own error on the narrowest lobe here.
    CHECK_NEAR(totalExpected / double(kSampleCount),
               double(h.kept) / double(kSampleCount), 0.01);

    // A failure names where it is, because a statistic alone says only that
    // something is wrong. The cells are listed by their own contribution,
    // before pooling, which is where a mismatched lobe shows its shape.
    if (!(chi.pValue > significance)) {
        std::vector<std::size_t> cells;
        for (std::size_t c = 0; c < h.expected.size(); ++c) {
            if (h.expected[c] >= 5.0 || h.observed[c] >= 5.0) cells.push_back(c);
        }
        const auto contribution = [&](std::size_t c) {
            const double e = std::max(h.expected[c], 1.0);
            return (h.observed[c] - e) * (h.observed[c] - e) / e;
        };
        std::sort(cells.begin(), cells.end(), [&](std::size_t l, std::size_t r) {
            return contribution(l) > contribution(r);
        });
        for (std::size_t i = 0; i < std::min<std::size_t>(cells.size(), 16); ++i) {
            const std::size_t c = cells[i];
            std::printf("  %-30s   cell (%5.1f, %5.1f deg)  observed %7.0f"
                        "  expected %9.1f\n",
                        "",
                        (double(c / kChiPhiBins) + 0.5) * 180.0 / kChiThetaBins,
                        (double(c % kChiPhiBins) + 0.5) * 360.0 / kChiPhiBins,
                        h.observed[c], h.expected[c]);
        }
    }
}

enum class GuideKind { Diffuse, Specular, Mixed };

/// The reconstruction guides a closure publishes (docs/dlss-integration.md 4).
///
/// What can be asserted exactly is asserted exactly: the sampling and
/// evaluation passes publish the same guides, bit for bit, because a guide is a
/// property of the surface and the view; a diffuse lobe publishes no specular
/// albedo and a specular one no diffuse albedo; the normal is the surface's.
/// How closely the specular albedo -- MaterialX's fitted directional albedo --
/// matches the albedo integrated from the response is printed beside the
/// furnace, because that is the fit's accuracy and not a property to gate on.
void CheckGuides(const char* label, const ChiSquareHistograms& h, GuideKind kind,
                 double closedFormDiffuse)
{
    if (!h.ok) return;
    const auto& g = h.guides;
    for (std::size_t k = 0; k < 10; ++k) {
        CHECK_EQ(g[k], g[k + 10]);
    }
    const double diffuse = (g[0] + g[1] + g[2]) / 3.0;
    const double specular = (g[3] + g[4] + g[5]) / 3.0;
    std::printf("  %-30s guides diffuse %.4f  specular %.4f  normal (%.3f, %.3f,"
                " %.3f)  alpha %.3f  integrated albedo %.4f\n",
                label, diffuse, specular, g[6], g[7], g[8], g[9],
                h.integratedAlbedo);
    if (kind == GuideKind::Diffuse) {
        CHECK_EQ(specular, 0.0);
        CHECK(diffuse > 0.0);
    } else if (kind == GuideKind::Specular) {
        CHECK_EQ(diffuse, 0.0);
        CHECK(specular > 0.0);
    } else {
        CHECK(diffuse + specular > 0.0);
    }
    CHECK_NEAR(g[6], 0.0, 1.0e-6);
    CHECK_NEAR(g[7], 0.0, 1.0e-6);
    CHECK_NEAR(g[8], 1.0, 1.0e-6);
    if (closedFormDiffuse >= 0.0) {
        CHECK_NEAR(diffuse, closedFormDiffuse, 1.0e-6);
    }
}

/// The specular albedo is the reflectivity the lobe actually renders with.
///
/// For an isotropic GGX reflection lobe the guide is MaterialX's directional
/// albedo with the same energy compensation the response carries, and it
/// should then be the integrated albedo -- held to the furnace's own 0.5%.
/// Anisotropic and transmissive lobes are not asked: MaterialX's albedo fit
/// takes one roughness, and transmission is reported at its tint.
void CheckSpecularAlbedo(const char* label, const ChiSquareHistograms& h)
{
    if (!h.ok) return;
    const double specular = (h.guides[3] + h.guides[4] + h.guides[5]) / 3.0;
    CHECK_NEAR(specular, h.integratedAlbedo, 0.005 * h.integratedAlbedo);
}

/// Item 2: the importance-sampled albedo is the albedo, to half a per cent.
///
/// The reference is the response itself integrated over the sphere by
/// quadrature -- the model as MaterialX defines it, energy compensation and
/// all, evaluated by the same generated code rather than a second transcription
/// that could disagree with it for reasons of its own. Where a closed form
/// exists it checks that reference too, so the instrument is not trusted on its
/// own word.
///
/// The measurement has to be able to resolve the gate before it can pass it,
/// so the sampled estimate's standard error is held to a sixth of the
/// tolerance first. A noisier estimate is a failure of the test, not a pass.
void CheckFurnace(const char* label, const ChiSquareHistograms& h,
                  double closedForm)
{
    if (!h.ok) return;
    constexpr double kTolerance = 0.005;

    const double reference = h.integratedAlbedo;
    const double difference = h.sampledAlbedo - reference;
    std::printf("  %-30s furnace %.5f +- %.5f  integrated %.5f  (%+.3f%%)\n",
                label, h.sampledAlbedo, h.sampledAlbedoError, reference,
                reference > 0.0 ? 100.0 * difference / reference : 0.0);

    CHECK(reference > 0.0);
    if (!(reference > 0.0)) return;
    CHECK(3.0 * h.sampledAlbedoError <= 0.5 * kTolerance * reference);
    CHECK(std::abs(difference) <= kTolerance * reference);
    if (closedForm >= 0.0) {
        CHECK_NEAR(reference, closedForm, kTolerance * closedForm);
        CHECK_NEAR(h.sampledAlbedo, closedForm, kTolerance * closedForm);
    }
}

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

        // --- Whole closures, asked the way `shade` asks -----------------------
        //
        // Every measurement above evaluates with REFLECTION, which is right for
        // the lobes and probes it was written for and says nothing about a
        // closure that scatters behind its normal. The integrator asks
        // TRANSMISSION there, so energy and probability mass are measured
        // again in that form for every closure that can send light through:
        // one that answers only REFLECTION -- which `chiang_hair_bsdf` did --
        // passes the checks above and loses half its light in a render.
        {
            // Two further expectations where a closed form gives one, because
            // energy and mass alone are satisfied by a closure that throws its
            // samples away: density 0.5 plus discarded 0.5 sums to one, and so
            // does 0 plus 1. White diffuse transmission returns everything it
            // receives, and a lobe sampled uniformly over a sphere it is
            // defined on everywhere has nothing to discard.
            constexpr double kNone = -1.0;
            struct WholeCase {
                const char* label;
                double tolerance;
                std::function<mx::NodePtr(mx::DocumentPtr)> build;
                double expectedAlbedo = kNone;
                double maxDiscarded = kNone;
            };
            const WholeCase whole[] = {
                {"dielectric RT, by side", kEnergyOnly,
                 [](mx::DocumentPtr doc) {
                     mx::NodePtr n = AddNode(doc, "dielectric_bsdf", "wD", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "ior", 1.5f);
                     SetValue(n, "roughness", mx::Vector2(0.3f, 0.3f));
                     n->setInputValue("scatter_mode", std::string("RT"), "string");
                     return n;
                 }},
                {"generalized_schlick RT, by side", kEnergyOnly,
                 [](mx::DocumentPtr doc) {
                     mx::NodePtr n =
                         AddNode(doc, "generalized_schlick_bsdf", "wG", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color0", mx::Color3(0.04f, 0.04f, 0.04f));
                     SetValue(n, "color90", mx::Color3(1.0f, 1.0f, 1.0f));
                     SetValue(n, "roughness", mx::Vector2(0.3f, 0.3f));
                     n->setInputValue("scatter_mode", std::string("RT"), "string");
                     return n;
                 }},
                {"translucent, by side", kBroadLobeTolerance,
                 [](mx::DocumentPtr doc) {
                     mx::NodePtr n = AddNode(doc, "translucent_bsdf", "wT", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
                     return n;
                 },
                 1.0, 0.0},
                {"chiang_hair, by side", kBroadLobeTolerance,
                 [](mx::DocumentPtr doc) {
                     return AddNode(doc, "chiang_hair_bsdf", "wH", "BSDF");
                 },
                 kNone, 0.0},
                {"multiply(conductor, 0.5)", kBroadLobeTolerance,
                 [](mx::DocumentPtr doc) {
                     mx::NodePtr c = AddNode(doc, "conductor_bsdf", "wMc", "BSDF");
                     SetValue(c, "weight", 1.0f);
                     SetValue(c, "roughness", mx::Vector2(0.4f, 0.4f));
                     mx::NodePtr m = AddNode(doc, "multiply", "wMul", "BSDF");
                     Connect(m, "in1", c);
                     SetValue(m, "in2", 0.5f);
                     return m;
                 }},
            };
            for (const WholeCase& c : whole) {
                mx::DocumentPtr doc = validator.NewDocument();
                mx::NodePtr bsdf = c.build(doc);
                const Measurement m = validator.Measure(
                    WrapInMaterial(doc, bsdf), "vWhole", 0.6f, kClosureBySide);
                CheckClosure(c.label, m, c.tolerance);
                if (c.expectedAlbedo != kNone) {
                    CHECK_NEAR(m.albedo, c.expectedAlbedo, 0.01);
                }
                if (c.maxDiscarded != kNone) {
                    CHECK(m.discardedFraction <= c.maxDiscarded);
                }
            }
        }

        // --- Chi-squared: sampling against the reported density --------------
        //
        // The last of the five acceptance items (materialx-codegen.md 8). The
        // furnace and the mass check above agree on totals; this asks where
        // the mass is, cell by cell over the sphere, so a sampler that puts the
        // right amount of energy in the wrong directions fails here and
        // nowhere else.
        //
        // The p-value is first checked against published chi-squared critical
        // values -- each is the statistic at which p is exactly 0.05 -- so the
        // gate below cannot pass because the survival function is wrong.
        CHECK_NEAR(RegularizedGammaQ(0.5, 0.5 * 3.841459), 0.05, 1.0e-5);
        CHECK_NEAR(RegularizedGammaQ(5.0, 0.5 * 18.307038), 0.05, 1.0e-5);
        CHECK_NEAR(RegularizedGammaQ(50.0, 0.5 * 124.342113), 0.05, 1.0e-5);
        {
            using Builder = std::function<mx::NodePtr(mx::DocumentPtr)>;
            constexpr double kNoClosedForm = -1.0;
            struct Case {
                const char* label;
                float viewTheta;
                Builder build;
                // Whether the furnace is held to 0.5% (item 2), and an albedo
                // known without measuring anything, where there is one.
                bool furnace = true;
                double closedForm = kNoClosedForm;
                std::uint32_t furnaceBatches = 1;
            };
            const auto diffuse = [](mx::DocumentPtr doc, const char* name,
                                    float grey) {
                mx::NodePtr n =
                    AddNode(doc, "oren_nayar_diffuse_bsdf", name, "BSDF");
                SetValue(n, "weight", 1.0f);
                SetValue(n, "color", mx::Color3(grey, grey, grey));
                return n;
            };
            const auto conductor = [](mx::DocumentPtr doc, const char* name,
                                      float weight, float ru, float rv) {
                mx::NodePtr n = AddNode(doc, "conductor_bsdf", name, "BSDF");
                SetValue(n, "weight", weight);
                SetValue(n, "roughness", mx::Vector2(ru, rv));
                return n;
            };
            const auto dielectric = [](mx::DocumentPtr doc, const char* name,
                                       float roughness, const char* mode) {
                mx::NodePtr n = AddNode(doc, "dielectric_bsdf", name, "BSDF");
                SetValue(n, "weight", 1.0f);
                SetValue(n, "ior", 1.5f);
                SetValue(n, "roughness", mx::Vector2(roughness, roughness));
                n->setInputValue("scatter_mode", std::string(mode), "string");
                return n;
            };

            const Case cases[] = {
                {"oren_nayar (smooth)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n = diffuse(doc, "xOs", 1.0f);
                     SetValue(n, "roughness", 0.0f);
                     return n;
                 },
                 true, 1.0},
                {"oren_nayar (rough 0.5)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n = diffuse(doc, "xOn", 1.0f);
                     SetValue(n, "roughness", 0.5f);
                     return n;
                 }},
                {"burley_diffuse (rough 0.5)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n =
                         AddNode(doc, "burley_diffuse_bsdf", "xBu", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
                     SetValue(n, "roughness", 0.5f);
                     return n;
                 }},
                {"conductor (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     return conductor(doc, "xCo", 1.0f, 0.3f, 0.3f);
                 }},
                {"conductor (0.2 x 0.6)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     return conductor(doc, "xCa", 1.0f, 0.2f, 0.6f);
                 }},
                {"dielectric R (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     return dielectric(doc, "xDr", 0.3f, "R");
                 }},
                {"dielectric RT (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     return dielectric(doc, "xDt", 0.3f, "RT");
                 }},
                {"sheen (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n = AddNode(doc, "sheen_bsdf", "xSh", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
                     SetValue(n, "roughness", 0.3f);
                     return n;
                 },
                 // Cosine-sampled under a lobe that reflects a tenth of the
                 // light, so a single batch leaves a standard error of a tenth
                 // of a per cent; eight bring it inside what 0.5% can resolve.
                 true, kNoClosedForm, 8},
                {"mix(conductor, diffuse)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr m = AddNode(doc, "mix", "xMix", "BSDF");
                     Connect(m, "fg", conductor(doc, "xMb", 1.0f, 0.4f, 0.4f));
                     Connect(m, "bg", diffuse(doc, "xMa", 1.0f));
                     SetValue(m, "mix", 0.5f);
                     return m;
                 }},
                {"layer(dielectric, diffuse)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr l = AddNode(doc, "layer", "xLay", "BSDF");
                     Connect(l, "top", dielectric(doc, "xLt", 0.2f, "R"));
                     Connect(l, "base", diffuse(doc, "xLb", 1.0f));
                     return l;
                 }},
                {"add(diffuse, conductor)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr s = AddNode(doc, "add", "xAdd", "BSDF");
                     Connect(s, "in1", diffuse(doc, "xAa", 0.5f));
                     Connect(s, "in2", conductor(doc, "xAb", 0.5f, 0.5f, 0.5f));
                     return s;
                 }},
                // Nested selections. Every combinator chooses a lobe in the
                // sampling pass, and children run before their parents, so
                // each nested choice must be independent of the ones around
                // it: the density a combinator reports is the product of the
                // selection probabilities along the way to each leaf, and it
                // describes the sampling only if those choices are. Every
                // case above holds a single choice and cannot see this.
                {"mix(mix(conductor, diffuse), conductor)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr inner = AddNode(doc, "mix", "xNi", "BSDF");
                     Connect(inner, "fg", conductor(doc, "xNa", 1.0f, 0.1f, 0.1f));
                     Connect(inner, "bg", diffuse(doc, "xNb", 1.0f));
                     SetValue(inner, "mix", 0.5f);
                     mx::NodePtr outer = AddNode(doc, "mix", "xNo", "BSDF");
                     Connect(outer, "fg", inner);
                     Connect(outer, "bg", conductor(doc, "xNc", 1.0f, 0.5f, 0.5f));
                     SetValue(outer, "mix", 0.5f);
                     return outer;
                 }},
                {"mix(dielectric RT, diffuse)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr m = AddNode(doc, "mix", "xNd", "BSDF");
                     Connect(m, "fg", dielectric(doc, "xNe", 0.3f, "RT"));
                     Connect(m, "bg", diffuse(doc, "xNf", 1.0f));
                     SetValue(m, "mix", 0.5f);
                     return m;
                 }},
                {"layer(dielectric R, mix(conductor, diffuse))", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr m = AddNode(doc, "mix", "xNg", "BSDF");
                     Connect(m, "fg", conductor(doc, "xNh", 1.0f, 0.1f, 0.1f));
                     Connect(m, "bg", diffuse(doc, "xNj", 1.0f));
                     SetValue(m, "mix", 0.3f);
                     mx::NodePtr l = AddNode(doc, "layer", "xNl", "BSDF");
                     Connect(l, "top", dielectric(doc, "xNk", 0.5f, "R"));
                     Connect(l, "base", m);
                     return l;
                 }},
                {"multiply(conductor, 0.5)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr m = AddNode(doc, "multiply", "xMul", "BSDF");
                     Connect(m, "in1", conductor(doc, "xMc", 1.0f, 0.3f, 0.3f));
                     SetValue(m, "in2", 0.5f);
                     return m;
                 }},
                {"dielectric T (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     return dielectric(doc, "xDT", 0.3f, "T");
                 }},
                {"generalized_schlick R (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n =
                         AddNode(doc, "generalized_schlick_bsdf", "xGr", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color0", mx::Color3(0.9f, 0.6f, 0.3f));
                     SetValue(n, "roughness", mx::Vector2(0.3f, 0.3f));
                     return n;
                 }},
                {"generalized_schlick RT (0.3)", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n =
                         AddNode(doc, "generalized_schlick_bsdf", "xGt", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color0", mx::Color3(0.04f, 0.04f, 0.04f));
                     SetValue(n, "color90", mx::Color3(1.0f, 1.0f, 1.0f));
                     SetValue(n, "roughness", mx::Vector2(0.3f, 0.3f));
                     n->setInputValue("scatter_mode", std::string("RT"), "string");
                     return n;
                 }},
                {"translucent", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n = AddNode(doc, "translucent_bsdf", "xTr", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color", mx::Color3(1.0f, 1.0f, 1.0f));
                     return n;
                 },
                 true, 1.0},
                {"subsurface", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     mx::NodePtr n = AddNode(doc, "subsurface_bsdf", "xSs", "BSDF");
                     SetValue(n, "weight", 1.0f);
                     SetValue(n, "color", mx::Color3(0.8f, 0.8f, 0.8f));
                     return n;
                 },
                 // Not in item 2's list, and not a furnace in the same sense:
                 // this lobe is the entry into a medium whose walk decides the
                 // albedo, which the render suite's sphere furnaces measure.
                 false},
                {"chiang_hair", 0.6f,
                 [&](mx::DocumentPtr doc) {
                     return AddNode(doc, "chiang_hair_bsdf", "xHa", "BSDF");
                 },
                 // Not in item 2's list. Sampled uniformly over the sphere, so
                 // its estimate is too noisy at this count to resolve 0.5%.
                 false},
            };

            // One per cent over the whole set, shared between the cases.
            const double significance =
                1.0 - std::pow(1.0 - 0.01, 1.0 / double(std::size(cases)));
            for (const Case& c : cases) {
                mx::DocumentPtr doc = validator.NewDocument();
                mx::NodePtr bsdf = c.build(doc);
                const ChiSquareHistograms h = validator.MeasureDistribution(
                    WrapInMaterial(doc, bsdf), "vChi2", c.viewTheta,
                    c.furnaceBatches);
                CheckDistribution(c.label, h, significance);
                if (c.furnace) {
                    CheckFurnace(c.label, h, c.closedForm);
                }
                const std::string label = c.label;
                const auto starts = [&](const char* prefix) {
                    return label.rfind(prefix, 0) == 0;
                };
                const GuideKind kind =
                    starts("oren_nayar") || starts("burley") ||
                            starts("translucent") || starts("subsurface")
                        ? GuideKind::Diffuse
                    : starts("mix(") || starts("layer(") || starts("add(")
                        ? GuideKind::Mixed
                        : GuideKind::Specular;
                // White smooth Oren-Nayar and white translucent have a diffuse
                // albedo of one, and an even mix of a conductor with white
                // diffuse has half of it.
                const double closedDiffuse =
                    label == "oren_nayar (smooth)" || label == "translucent" ? 1.0
                    : label == "mix(conductor, diffuse)"                     ? 0.5
                                                                              : -1.0;
                CheckGuides(c.label, h, kind, closedDiffuse);
                if (label == "conductor (0.3)" ||
                    label == "generalized_schlick R (0.3)" ||
                    label == "multiply(conductor, 0.5)") {
                    CheckSpecularAlbedo(c.label, h);
                }
            }
        }

        // --- The dielectric interface's own directional albedo ---------------
        //
        // What open question 10 needs and what guessing it got wrong. A rough
        // dielectric loses light -- a closed sphere of one reads 0.88 at alpha
        // 0.3 and 0.63 at 0.6 where it must read one -- because single
        // scattering drops every ray that leaves into another microfacet, and
        // MaterialX compensates only the reflection lobe. Restoring the rest
        // means scaling by the interface's albedo, so the first thing to know
        // is what that albedo *is*.
        //
        // An attempt at deriving it from `mx_ggx_dir_albedo`, which is a
        // reflection-only fit, overshot by three times the deficit: it assumed
        // the transmission lobe loses as much to masking as the reflection lobe
        // does at the same roughness. This measures both instead. The quadrature
        // integrates the response over the *whole* sphere, evaluating each
        // direction as the shade kernel would -- reflection above the surface,
        // transmission below -- so an RT lobe's integrated albedo is the whole
        // interface and an R lobe's is its reflection half. Their difference is
        // the transmission half.
        //
        // Off by default, because it is a generator rather than a gate: a grid
        // of these is what a table would be built from, and nothing asserts
        // them yet. `HDCLAUDE_ALBEDO_GRID=1` prints it.
        const char* const albedoGrid = std::getenv("HDCLAUDE_ALBEDO_GRID");
        if (albedoGrid != nullptr && albedoGrid[0] != '\0' &&
            albedoGrid[0] != '0') {
            std::printf("\n  dielectric interface albedo "
                        "(quadrature, entering)\n");
            // `Ess` beside them, which is the same GGX lobe with its Fresnel
            // held at one -- `generalized_schlick` with both colours white --
            // and so is the energy a *mirror* of this roughness keeps. It is
            // the quantity MaterialX's own compensation is built from, and
            // printing it here is what says whether the interface's deficit is
            // that deficit wearing a different coat or something else entirely.
            std::printf("  %-6s %-6s %-8s  %-8s %-8s %-8s %-8s\n", "ior",
                        "alpha", "theta", "E_total", "E_R", "E_T", "Ess");
            for (const float ior : {1.33f, 1.5f, 2.0f}) {
                for (const float alpha : {0.1f, 0.3f, 0.6f}) {
                    for (const float viewTheta : {0.2f, 0.6f, 1.0f}) {
                        const auto measure = [&](const char* mode,
                                                 const char* tag) {
                            mx::DocumentPtr doc = validator.NewDocument();
                            mx::NodePtr n =
                                AddNode(doc, "dielectric_bsdf", tag, "BSDF");
                            SetValue(n, "weight", 1.0f);
                            SetValue(n, "ior", ior);
                            SetValue(n, "roughness",
                                     mx::Vector2(alpha, alpha));
                            n->setInputValue("scatter_mode", std::string(mode),
                                             "string");
                            return validator
                                .MeasureDistribution(WrapInMaterial(doc, n),
                                                     std::string("vAlb") + tag,
                                                     viewTheta, 4)
                                .integratedAlbedo;
                        };
                        const double total = measure("RT", "aRT");
                        const double reflected = measure("R", "aR");

                        mx::DocumentPtr mirrorDoc = validator.NewDocument();
                        mx::NodePtr mirror = AddNode(
                            mirrorDoc, "generalized_schlick_bsdf", "aE", "BSDF");
                        SetValue(mirror, "weight", 1.0f);
                        SetValue(mirror, "color0",
                                 mx::Color3(1.0f, 1.0f, 1.0f));
                        SetValue(mirror, "color82",
                                 mx::Color3(1.0f, 1.0f, 1.0f));
                        SetValue(mirror, "color90",
                                 mx::Color3(1.0f, 1.0f, 1.0f));
                        SetValue(mirror, "roughness",
                                 mx::Vector2(alpha, alpha));
                        const double ess =
                            validator
                                .MeasureDistribution(
                                    WrapInMaterial(mirrorDoc, mirror), "vAlbE",
                                    viewTheta, 4)
                                .integratedAlbedo;

                        std::printf("  %-6.2f %-6.2f %-8.2f  %-8.4f %-8.4f "
                                    "%-8.4f %-8.4f\n",
                                    double(ior), double(alpha),
                                    double(viewTheta), total, reflected,
                                    total - reflected, ess);
                    }
                }
            }
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
