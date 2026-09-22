#include "material_compiler.h"

#include "hdclaude/core/environment.h"

#include "trace.h"

#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/HwShaderGenerator.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"

#include <filesystem>
#include <map>
#include <fstream>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace mx = MaterialX;

namespace {

/// Node types hdClaude shades through MaterialX.
///
/// A terminal outside this set is not a MaterialX surface and is reported
/// rather than translated.
///
/// `UsdPreviewSurface` belongs here, which is not the exception it looks like.
/// MaterialX declares `ND_UsdPreviewSurface_surfaceshader` and implements it as
/// a *nodegraph* of ordinary MaterialX nodes, so a USD-native material is
/// generated and executed by the same path as every other one -- no extractor,
/// no surface-model special case, no third approximated state
/// (docs/architecture.md 1.1). Refusing it and shading `displayColor` instead
/// was not fidelity; it was leaving MaterialX's own translation unused, and it
/// cost every material in a USD-authored asset. Intel Sponza has 137 of them.
bool IsMaterialXSurface(const TfToken& nodeType)
{
    static const TfToken kNdPrefix("ND_");
    const std::string& name = nodeType.GetString();
    return name.rfind(kNdPrefix.GetString(), 0) == 0 ||
           name == "standard_surface" || name == "open_pbr_surface" ||
           name == "surface" || name == "gltf_pbr" ||
           name == "UsdPreviewSurface" ||
           name == "UsdPreviewSurface_to_MaterialX";
}

/// The asset path behind a generated texture uniform.
///
/// The generator names its samplers after the MaterialX node and input they
/// came from -- `mtlximage1_file` for the `file` input of node `mtlximage1` --
/// so the document is searched for the input whose variable name the generator
/// used. Going back to the document rather than trusting the uniform's baked
/// default matters because hdMtlx resolves asset paths as it builds it.
/// Resolved asset paths, keyed by the uniform name the generator will produce.
///
/// hdMtlx writes the *authored* asset path into the document -- it calls
/// SdfAssetPath::GetAssetPath -- so a relative path stays relative and no
/// resolver can anchor it afterwards. The Hydra network is where the resolved
/// path lives, because USD resolved it against the layer that authored it.
///
/// The join is not a guess: hdMtlx names a MaterialX node
/// HdMtlxCreateNameFromPath(hdNodePath), which is the path's last element, and
/// MaterialX names a sampler uniform after its node and input. Both halves are
/// public API, so this reproduces the name rather than pattern-matching it.
std::map<std::string, std::string> ResolvedTexturePaths(
    const HdMaterialNetwork2& network, const HdMtlxTexturePrimvarData& mxHdData)
{
    std::map<std::string, std::string> resolved;
    for (const SdfPath& texturePath : mxHdData.hdTextureNodes) {
        const auto node = network.nodes.find(texturePath);
        if (node == network.nodes.end()) {
            continue;
        }
        const std::string mxNodeName = HdMtlxCreateNameFromPath(texturePath);
        for (const auto& [parameter, value] : node->second.parameters) {
            if (!value.IsHolding<SdfAssetPath>()) {
                continue;
            }
            const SdfAssetPath& asset = value.UncheckedGet<SdfAssetPath>();
            std::string path = asset.GetResolvedPath();
            if (path.empty()) {
                path = asset.GetAssetPath();
            }
            resolved[mx::createValidName(mxNodeName + "_" +
                                         parameter.GetString())] = path;
        }
    }
    return resolved;
}

/// How the bytes behind an <image> node are to be read.
///
/// MaterialX and USD each say this a different way and an asset uses whichever
/// its exporter wrote:
///
///   * a `colorspace` attribute on the `file` input -- what a MaterialX
///     document authors, and the only one of the three that is explicit;
///   * a `sourceColorSpace` input -- what a `UsdUVTexture` authors, and what
///     hdMtlx carries across as an input rather than as an attribute;
///   * nothing at all, in which case the node's *type* is the answer. An
///     `image` returning `color3` or `color4` is colour, and an 8-bit file
///     holding it is sRGB-encoded; one returning `float`, `vector2` or
///     `vector3` is data, and decoding data as sRGB is what turns a flat
///     normal map into a tilt and a subtle bump into a dent.
///
/// The chess set relies on the third: its normal, roughness and metalness maps
/// are JPEGs on `vector3` and `float` nodes with no colour space anywhere.
HdClaudeTextureColorSpace ImageColorSpace(const mx::NodePtr& node,
                                          const mx::InputPtr& fileInput)
{
    // Non-inherited on purpose. A document routinely declares its *working*
    // space at the top -- the chess set says `colorspace="lin_rec709"` on
    // <materialx> -- and reading that as the file's encoding would call every
    // texture in it linear.
    std::string declared = fileInput->getAttribute(mx::Element::COLOR_SPACE_ATTRIBUTE);
    if (declared.empty()) {
        if (const mx::InputPtr source = node->getInput("sourceColorSpace")) {
            declared = source->getValueString();
        }
    }
    if (!declared.empty() && declared != "auto") {
        // "auto" is `UsdUVTexture`'s default and its own word for "the file
        // decides", so it is deliberately not a declaration; reading it as one
        // would call every UsdPreviewSurface texture linear.
        //
        // `g22_rec709` is gamma 2.2 rather than the sRGB curve exactly, but the
        // two differ only in the bottom of the ramp and the hardware decode is
        // much closer to it than no decode at all.
        return (declared == "srgb_texture" || declared == "sRGB" ||
                declared == "g22_rec709")
                   ? HdClaudeTextureColorSpace::Srgb
                   : HdClaudeTextureColorSpace::Raw;
    }

    const std::string& type = node->getType();
    if (type == "color3" || type == "color4") {
        return HdClaudeTextureColorSpace::Auto;
    }
    if (type == "multioutput") {
        // A `UsdUVTexture` with `sourceColorSpace` left at its "auto" default,
        // which USD defines as "the file decides".
        return HdClaudeTextureColorSpace::Auto;
    }
    return HdClaudeTextureColorSpace::Raw;
}

/// The UDIM tiles a `<UDIM>` path actually has, in ascending order.
///
/// Asked of the asset resolver rather than assumed, over the 10x10 grid UDIM
/// defines. A set is not required to be contiguous or to start at 1001 -- ALab's
/// turntable ships 1001 to 1007 and then 1013, and the OpenPBR playground's
/// tools start at 1003 -- so the only way to know is to ask for each of the
/// hundred.
///
/// Returns empty for a path with no `<UDIM>` token, which is every ordinary
/// texture, and for a set whose every tile is missing; the caller then treats
/// the path as a single image and the loader reports it if it cannot be read.
std::vector<int> ResolveUdimTiles(const std::string& assetPath)
{
    static const std::string kToken = "<UDIM>";
    const std::size_t token = assetPath.find(kToken);
    if (token == std::string::npos) {
        return {};
    }

    std::vector<int> tiles;
    for (int tile = 1001; tile <= 1100; ++tile) {
        std::string candidate = assetPath;
        candidate.replace(token, kToken.size(), std::to_string(tile));
        if (ArGetResolver().Resolve(candidate)) {
            tiles.push_back(tile);
        }
    }
    return tiles;
}

/// One entry of `path` with `<UDIM>` replaced by `tile`.
std::string SubstituteUdimTile(const std::string& path, int tile)
{
    static const std::string kToken = "<UDIM>";
    const std::size_t token = path.find(kToken);
    if (token == std::string::npos || tile <= 0) {
        return path;
    }
    std::string substituted = path;
    substituted.replace(token, kToken.size(), std::to_string(tile));
    return substituted;
}

HdClaudeMaterialCompiler::TextureRequest ResolveTexturePath(
    const mx::DocumentPtr& document, const std::string& uniformName)
{
    // The whole tree, not just the document's own children: hdMtlx wraps a
    // material's pattern nodes in a nodegraph, so every <image> in a real
    // asset is a grandchild rather than a child.
    for (const mx::ElementPtr& element : document->traverseTree()) {
        mx::NodePtr node = element ? element->asA<mx::Node>() : nullptr;
        if (!node) {
            continue;
        }
        for (const mx::InputPtr& input : node->getInputs()) {
            if (input->getType() != "filename") {
                continue;
            }
            // MaterialX builds the uniform name by joining the node and input
            // names with an underscore, after replacing characters GLSL cannot
            // take. Comparing on that join is what ties the two together.
            const std::string candidate =
                mx::createValidName(node->getName() + "_" + input->getName());
            if (candidate == uniformName) {
                HdClaudeMaterialCompiler::TextureRequest request;
                request.path = input->getResolvedValueString();
                request.colorSpace = ImageColorSpace(node, input);
                return request;
            }
        }
    }
    return HdClaudeMaterialCompiler::TextureRequest();
}

/// The MaterialX nodedef behind a USD-native shader node type.
///
/// `HdMtlxCreateMtlxDocumentFromHdNetwork` looks a node's type up as a
/// MaterialX nodedef name, which is what a MaterialX-authored network already
/// carries. A USD-native one carries USD's own names -- `UsdPreviewSurface`,
/// `UsdUVTexture` -- and hdMtlx cannot map them, so it emits nodes with no
/// category and no definition and generation fails on the first of them.
///
/// MaterialX declares every one of these itself, with the same input names, so
/// this is a rename and nothing more: the shading is still MaterialX's, built
/// from its own `IMP_UsdPreviewSurface_surfaceshader` nodegraph. Anything not
/// in this table keeps its type and is reported if it cannot be generated.
const std::map<TfToken, TfToken>& UsdNodeTypeTranslations()
{
    static const std::map<TfToken, TfToken> kTranslations = {
        {TfToken("UsdPreviewSurface"), TfToken("ND_UsdPreviewSurface_surfaceshader")},
        {TfToken("UsdUVTexture"), TfToken("ND_UsdUVTexture")},
        {TfToken("UsdTransform2d"), TfToken("ND_UsdTransform2d")},
        {TfToken("UsdPrimvarReader_float"), TfToken("ND_UsdPrimvarReader_float")},
        {TfToken("UsdPrimvarReader_float2"), TfToken("ND_UsdPrimvarReader_vector2")},
        {TfToken("UsdPrimvarReader_float3"), TfToken("ND_UsdPrimvarReader_vector3")},
        {TfToken("UsdPrimvarReader_float4"), TfToken("ND_UsdPrimvarReader_vector4")},
        {TfToken("UsdPrimvarReader_normal"), TfToken("ND_UsdPrimvarReader_vector3")},
        {TfToken("UsdPrimvarReader_point"), TfToken("ND_UsdPrimvarReader_vector3")},
        {TfToken("UsdPrimvarReader_vector"), TfToken("ND_UsdPrimvarReader_vector3")},
        {TfToken("UsdPrimvarReader_int"), TfToken("ND_UsdPrimvarReader_integer")},
        {TfToken("UsdPrimvarReader_string"), TfToken("ND_UsdPrimvarReader_string")},
        {TfToken("UsdPrimvarReader_matrix"), TfToken("ND_UsdPrimvarReader_matrix44")},
    };
    return kTranslations;
}

/// Rewrite a network's USD-native node types into their MaterialX nodedefs.
///
/// The network is copied rather than edited: it belongs to the caller, and a
/// material that cannot be translated must be reported against what the stage
/// actually authored.
HdMaterialNetwork2 TranslateUsdNodeTypes(const HdMaterialNetwork2& network)
{
    HdMaterialNetwork2 translated = network;
    for (auto& [path, node] : translated.nodes) {
        const auto found = UsdNodeTypeTranslations().find(node.nodeTypeId);
        if (found == UsdNodeTypeTranslations().end()) {
            continue;
        }
        node.nodeTypeId = found->second;

        // The wrap modes are spelled differently on the two sides. USD says
        // `repeat` and MaterialX's enum says `periodic`, and a value outside
        // the enum stops generation for the whole material -- which for a
        // textured asset is every material in it. `useMetadata` asks the image
        // file what to do, which hdClaude cannot answer, so it takes the
        // MaterialX default rather than refusing the material over it.
        if (node.nodeTypeId != TfToken("ND_UsdUVTexture")) {
            continue;
        }
        for (const TfToken& wrap : {TfToken("wrapS"), TfToken("wrapT")}) {
            const auto parameter = node.parameters.find(wrap);
            if (parameter == node.parameters.end()) {
                continue;
            }
            std::string value;
            if (parameter->second.IsHolding<TfToken>()) {
                value = parameter->second.UncheckedGet<TfToken>().GetString();
            } else if (parameter->second.IsHolding<std::string>()) {
                value = parameter->second.UncheckedGet<std::string>();
            } else {
                continue;
            }
            if (value == "repeat" || value == "useMetadata") {
                parameter->second = VtValue(std::string("periodic"));
            } else {
                parameter->second = VtValue(value);
            }
        }
    }
    return translated;
}

/// Turn `UsdPrimvarReader` nodes into the `geompropvalue` nodes they wrap.
///
/// MaterialX implements `ND_UsdPrimvarReader_*` as a nodegraph containing a
/// `geompropvalue` whose `geomprop` input is connected to the graph's
/// `varname` interface. The GLSL implementation of `geompropvalue` reads that
/// input's *value* to know which primvar to declare, and an interface
/// connection is not a value, so generation stops with
///
///     No 'geomprop' parameter found on geompropvalue node 'primvar'.
///     Don't know what property to bind
///
/// which is one node costing a whole scene -- Collective Project 001 in this
/// gallery. The node is rewritten in place rather than replaced, so every
/// connection into and out of it survives untouched.
void ResolvePrimvarReaders(const mx::DocumentPtr& document, const std::string& name)
{
    std::vector<mx::NodePtr> readers;
    for (const mx::ElementPtr& element : document->traverseTree()) {
        mx::NodePtr node = element ? element->asA<mx::Node>() : nullptr;
        if (node && node->getCategory() == "UsdPrimvarReader") {
            readers.push_back(node);
        }
    }

    for (const mx::NodePtr& node : readers) {
        const mx::InputPtr varname = node->getInput("varname");
        const std::string primvar = varname ? varname->getValueString() : "";
        if (primvar.empty()) {
            TF_WARN(
                "hdClaude: material %s: '%s' reads a primvar it does not name; "
                "it is left alone and will fail to generate",
                name.c_str(), node->getName().c_str());
            continue;
        }

        const std::string type = node->getType();
        node->setCategory("geompropvalue");
        node->setNodeDefString("ND_geompropvalue_" + type);
        node->removeInput("varname");

        if (const mx::InputPtr geomprop = node->addInput("geomprop", "string")) {
            geomprop->setValueString(primvar);
        }
        // `fallback` and `default` are the same input under two names.
        if (const mx::InputPtr fallback = node->getInput("fallback")) {
            const std::string value = fallback->getValueString();
            node->removeInput("fallback");
            if (!value.empty()) {
                if (const mx::InputPtr fallbackValue =
                        node->addInput("default", type)) {
                    fallbackValue->setValueString(value);
                }
            }
        }
    }
}

/// Drop inputs the node's declaration does not have.
///
/// A stage authored against a newer MaterialX than the one hdClaude links
/// carries inputs that do not exist here -- the OpenPBR playground authors
/// `geometry_opacity` on `open_pbr_surface`, which 1.39.3 does not declare.
/// MaterialX then cannot resolve the node's definition at all, and a single
/// unknown input costs the whole material and, because a material that fails
/// to compile is reported rather than approximated, the whole scene.
///
/// Removing the input renders the material as the rest of its authored values
/// say, which is what every other renderer does with an input it does not
/// recognise. Each one is named in a warning: this is a version mismatch worth
/// knowing about, not something to swallow.
void PruneUndeclaredInputs(const mx::DocumentPtr& document, const std::string& name)
{
    for (const mx::ElementPtr& element : document->traverseTree()) {
        mx::NodePtr node = element ? element->asA<mx::Node>() : nullptr;
        if (!node) {
            continue;
        }
        mx::NodeDefPtr definition = node->getNodeDef();
        if (!definition) {
            // The usual lookup fails *because* of the bad input, so the
            // declaration is found by category and output type instead.
            for (const mx::NodeDefPtr& candidate :
                 document->getMatchingNodeDefs(node->getCategory())) {
                if (candidate->getType() == node->getType()) {
                    definition = candidate;
                    break;
                }
            }
        }
        if (!definition) {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> mismatched;
        for (const mx::InputPtr& input : node->getInputs()) {
            const mx::InputPtr declared =
                definition->getActiveInput(input->getName());
            if (!declared) {
                mismatched.emplace_back(input->getName(), "no such input");
            } else if (declared->getType() != input->getType()) {
                // A renamed *type* is the other half of the same problem: the
                // playground authors `geometry_opacity` as a colour where
                // 1.39.3 declares a float, and a type that disagrees stops the
                // node from resolving just as surely as a name that does not
                // exist.
                mismatched.emplace_back(
                    input->getName(),
                    "declared " + declared->getType() + ", authored " +
                        input->getType());
            }
        }
        for (const auto& [input, reason] : mismatched) {
            TF_WARN(
                "hdClaude: material %s: node '%s' input '%s' does not match "
                "MaterialX %s (%s); it is dropped and the rest of the node is "
                "shaded as authored",
                name.c_str(), node->getName().c_str(), input.c_str(),
                mx::getVersionString().c_str(), reason.c_str());
            node->removeInput(input);
        }
    }
}

/// Tell the generator which UDIM tiles each `filename` input resolves to.
///
/// This has to happen *before* generation, because a UDIM set takes one array
/// slot per tile and the generated handle carries how many. `udimSubstitution`
/// keeps every set found, including the ones of a single tile: such a set does
/// not change the generated code but its path still carries the token and
/// still has to be expanded, or the loader is left to guess which tile was
/// meant.
///
/// Shared by a material's two programs. Each generates separately and numbers
/// its own samplers from zero, and each has to be told about its own images.
void TellGeneratorAboutUdimSets(
    const mx::DocumentPtr& document, const std::string& name,
    const std::map<std::string, std::string>* resolvedTextures,
    mx::ShaderGenerator* generator,
    std::map<std::string, std::vector<int>>* udimSubstitution)
{
    auto* ptGenerator =
        dynamic_cast<hdclaude::PathTracerShaderGenerator*>(generator);
    if (!ptGenerator || !document || !udimSubstitution) {
        return;
    }

    std::map<std::string, std::vector<int>> udim;
    for (const mx::ElementPtr& element : document->traverseTree()) {
        mx::NodePtr node = element ? element->asA<mx::Node>() : nullptr;
        if (!node) {
            continue;
        }
        for (const mx::InputPtr& input : node->getInputs()) {
            if (input->getType() != "filename") {
                continue;
            }
            // Keyed by the uniform name MaterialX will build from the node and
            // input names -- the same join `ResolveTexturePath` matches on, so
            // the two cannot disagree about which image is which.
            const std::string uniform = mx::createValidName(
                node->getName() + "_" + input->getName());
            std::string path = input->getResolvedValueString();
            if (resolvedTextures) {
                const auto found = resolvedTextures->find(uniform);
                if (found != resolvedTextures->end() && !found->second.empty()) {
                    path = found->second;
                }
            }
            std::vector<int> tiles = ResolveUdimTiles(path);
            if (tiles.empty()) {
                continue;
            }
            HdClaudeTrace("material %s: '%s' is a UDIM set of %zu tiles "
                          "(%d..%d)",
                          name.c_str(), uniform.c_str(), tiles.size(),
                          tiles.front(), tiles.back());
            (*udimSubstitution)[uniform] = tiles;
            if (tiles.size() >= 2) {
                udim[uniform] = std::move(tiles);
            }
        }
    }
    ptGenerator->SetUdimTiles(std::move(udim));
}

/// The images a generated program samples, in the order it assigned them.
///
/// The generator is the authority on that order, so taking it from the
/// generator rather than re-deriving it from the document is what keeps the
/// two from drifting apart.
void CollectTexturePaths(
    const mx::DocumentPtr& document, const std::string& name,
    const std::map<std::string, std::string>* resolvedTextures,
    mx::ShaderGenerator* generator,
    const std::map<std::string, std::vector<int>>& udimSubstitution,
    std::vector<HdClaudeMaterialCompiler::TextureRequest>* texturePaths)
{
    if (!texturePaths) {
        return;
    }
    auto* ptGenerator =
        dynamic_cast<hdclaude::PathTracerShaderGenerator*>(generator);
    if (!ptGenerator) {
        return;
    }

    for (const auto& slot : ptGenerator->TextureOrder()) {
        const std::string& uniform = slot.uniform;
        // The colour space always comes from the document -- it is a property
        // of the <image> node, which the network does not carry -- while the
        // path prefers the network's resolved one and falls back to the
        // document's authored one, which is what a material hdClaude built
        // itself has.
        HdClaudeMaterialCompiler::TextureRequest request =
            ResolveTexturePath(document, uniform);
        if (resolvedTextures) {
            const auto found = resolvedTextures->find(uniform);
            if (found != resolvedTextures->end() && !found->second.empty()) {
                request.path = found->second;
            }
        }
        // One slot per tile, in the order the generator assigned them, which
        // is ascending tile number. A set of one tile has no tile on its slot
        // and is substituted from the scan.
        int tile = slot.tile;
        if (tile == 0) {
            const auto single = udimSubstitution.find(uniform);
            if (single != udimSubstitution.end() &&
                single->second.size() == 1) {
                tile = single->second.front();
            }
        }
        request.path = SubstituteUdimTile(request.path, tile);
        if (request.path.empty()) {
            // An image node with no file at all. That is legal and common --
            // an asset authors the node and leaves the file to a stronger
            // opinion that never arrives -- and MaterialX says such a node
            // returns its `default`. The pool turns the empty path into that
            // value, so this is worth tracing and not worth warning about; a
            // file that is authored and cannot be read is the case that
            // deserves to be loud.
            HdClaudeTrace("material %s samples '%s', which no asset path "
                          "backs; it reads the image node's default",
                          name.c_str(), uniform.c_str());
        }
        texturePaths->push_back(std::move(request));
    }
}

}  // namespace

HdClaudeMaterialCompiler::HdClaudeMaterialCompiler(std::string shadeKernel,
                                                   std::string displaceKernel)
    : _shadeKernel(std::move(shadeKernel)),
      _displaceKernel(std::move(displaceKernel))
{
    try {
        _libraries = hdclaude::LoadDefaultMaterialXLibraries();
    } catch (const std::exception& error) {
        TF_RUNTIME_ERROR("hdClaude: could not load the MaterialX libraries: %s",
                         error.what());
        _libraries = nullptr;
    }
}

hdclaude::CompiledMaterial HdClaudeMaterialCompiler::CompileDocument(
    mx::DocumentPtr document, const std::string& name, std::string* error,
    std::vector<TextureRequest>* texturePaths,
    const std::map<std::string, std::string>* resolvedTextures)
{
    // Caller holds _mutex: this touches the shared library document, the
    // generator, and glslang, none of which are thread-safe.
    hdclaude::CompiledMaterial result;
    result.debugName = name;
    // Read before generation, because generation is where MaterialX discards
    // it. The value travels beside the program instead of inside it, and
    // anything the document gets wrong about it is reported rather than
    // guessed at.
    std::vector<std::string> dispersionDiagnostics;
    result.dispersionAbbe =
        hdclaude::AuthoredDispersion(document, &dispersionDiagnostics);
    result.thinWalled =
        hdclaude::AuthoredThinWalled(document, &dispersionDiagnostics);
    // A thin sheet does not refract, so there is no direction for dispersion to
    // spread and nothing for it to do. Said once rather than silently ignored,
    // because an author who set both asked for something the model cannot give.
    if (result.thinWalled && result.dispersionAbbe > 0.0f) {
        dispersionDiagnostics.push_back(
            "authors transmission dispersion on a thin-walled surface, which "
            "does not refract; the dispersion has no effect");
        result.dispersionAbbe = 0.0f;
    }
    for (const std::string& diagnostic : dispersionDiagnostics) {
        TF_WARN("hdClaude: material %s: %s", name.c_str(), diagnostic.c_str());
    }

    try {
        mx::ShaderGeneratorPtr generator =
            hdclaude::PathTracerShaderGenerator::create();
        mx::GenContext context(generator);
        context.registerSourceCodeSearchPath(
            hdclaude::DefaultMaterialXSourceSearchPath());
        // Values are baked in rather than driven through a uniform block. The
        // block would be the better long-term answer -- it lets a parameter
        // change avoid a recompile -- but it needs std140 offsets computed from
        // the reflected block, and a wrong offset is a silently wrong material.
        // Recorded in docs/roadmap.md rather than guessed at.
        context.getOptions().shaderInterfaceType = mx::SHADER_INTERFACE_REDUCED;

        // Tiles per uniform, kept for the expansion below as well as for the
        // generator: a set of one tile does not change the generated code but
        // still has to have its token expanded before the loader sees it.
        std::map<std::string, std::vector<int>> udimSubstitution;

        // The tiles have to be known *before* generation, because a UDIM set
        // takes one array slot per tile and the generated handle carries how
        // many. Keyed by the uniform name MaterialX will build from the node
        // and input names -- the same join `ResolveTexturePath` matches on, so
        // the two cannot disagree about which image is which.
        TellGeneratorAboutUdimSets(document, name, resolvedTextures,
                                   generator.get(), &udimSubstitution);

        const std::vector<mx::TypedElementPtr> renderable =
            mx::findRenderableElements(document);
        if (renderable.empty()) {
            if (error) *error = "no renderable element in the MaterialX document";
            return result;
        }

        // The document goes out *before* generation, not after it. A material
        // that fails to generate is exactly the one worth reading, and until
        // this moved the dump only ever contained documents that had already
        // succeeded.
        if (const std::string dumpDir = hdclaude::EnvironmentValue("HDCLAUDE_DUMP_SHADERS");
            !dumpDir.empty()) {
            std::error_code code;
            std::filesystem::create_directories(dumpDir, code);
            mx::writeToXmlFile(document, (std::filesystem::path(dumpDir) /
                                          (name + ".mtlx")).string());
        }

        mx::ShaderPtr shader =
            generator->generate(name, renderable.front(), context);
        if (!shader) {
            if (error) *error = "MaterialX generated no shader";
            return result;
        }

        const std::string generated = shader->getSourceCode(mx::Stage::PIXEL);

        // HDCLAUDE_DUMP_SHADERS=<dir> writes every generated material there,
        // before the support checks, so a material that is refused can still
        // be read. Generated code is the one artefact in this pipeline nobody
        // ever sees unless it fails to compile, and by then the compiler
        // message is about a symbol rather than about what was generated.
        if (const std::string dumpDir = hdclaude::EnvironmentValue("HDCLAUDE_DUMP_SHADERS");
            !dumpDir.empty()) {
            std::error_code code;
            std::filesystem::create_directories(dumpDir, code);
            std::ofstream out(std::filesystem::path(dumpDir) /
                              (name + ".comp.glsl"));
            out << generated;
        }

        // The textures this material samples, in the order the generator
        // assigned their array indices. The caller loads them and publishes
        // them at those indices; the generator is the authority on the order,
        // so the two cannot drift apart.
        CollectTexturePaths(document, name, resolvedTextures, generator.get(),
                            udimSubstitution, texturePaths);

        const std::string source = generated + _shadeKernel;

        hdclaude::GlslCompileOptions options;
        options.moduleName = name;
        const hdclaude::GlslCompileResult compiled =
            _compiler.Compile(source, options);
        if (!compiled.ok) {
            if (error) *error = "shader compilation failed: " + compiled.log;
            return result;
        }
        result.spirv = compiled.spirv;
    } catch (const std::exception& exception) {
        if (error) *error = std::string("generation failed: ") + exception.what();
    }
    return result;
}

bool HdClaudeMaterialCompiler::CompileDisplacement(
    mx::DocumentPtr document, const std::string& name,
    hdclaude::CompiledMaterial* material, std::string* error,
    std::vector<TextureRequest>* texturePaths,
    const std::map<std::string, std::string>* resolvedTextures)
{
    // Caller holds _mutex, as for CompileDocument and for the same reasons.
    if (!material) {
        return true;
    }

    // Which terminal, and what its three floats mean. Both are facts about the
    // document that generation erases, so they are read first.
    std::vector<std::string> diagnostics;
    const hdclaude::DisplacementTerminal terminal =
        hdclaude::AuthoredDisplacement(document, &diagnostics);
    for (const std::string& diagnostic : diagnostics) {
        TF_WARN("hdClaude: material %s: %s", name.c_str(), diagnostic.c_str());
    }
    if (!terminal.terminal) {
        // The ordinary case. Most materials do not displace, and saying so is
        // not a diagnostic -- but it is worth tracing, because "this material
        // does not displace" and "this material's displacement did not
        // arrive" look identical from the outside.
        HdClaudeTrace(
            "material %s: the displacement document names no terminal, so "
            "nothing displaces",
            name.c_str());
        return true;
    }

    if (_displaceKernel.empty()) {
        if (error) {
            *error =
                "this build has no displace kernel (shaders/displace.comp.glsl "
                "was not found beside the plugin), so the material's "
                "displacement cannot be compiled";
        }
        return false;
    }

    try {
        mx::ShaderGeneratorPtr generator =
            hdclaude::PathTracerShaderGenerator::create();
        mx::GenContext context(generator);
        context.registerSourceCodeSearchPath(
            hdclaude::DefaultMaterialXSourceSearchPath());
        context.getOptions().shaderInterfaceType = mx::SHADER_INTERFACE_REDUCED;

        // Its own scan and its own texture order. The two programs generate
        // separately and each numbers its samplers from zero, so a height map
        // is index 0 of the displacement whatever the surface reads.
        std::map<std::string, std::vector<int>> udimSubstitution;
        TellGeneratorAboutUdimSets(document, name, resolvedTextures,
                                   generator.get(), &udimSubstitution);

        // Generated from the terminal by name. `findRenderableElements`
        // answers with surfaces, and a material that only displaces is not
        // one -- which is precisely the case this exists for.
        mx::ShaderPtr shader =
            generator->generate(name + "_displace", terminal.terminal, context);
        if (!shader) {
            if (error) *error = "MaterialX generated no displacement shader";
            return false;
        }

        const std::string generated = shader->getSourceCode(mx::Stage::PIXEL);
        if (const std::string dumpDir = hdclaude::EnvironmentValue("HDCLAUDE_DUMP_SHADERS");
            !dumpDir.empty()) {
            std::error_code code;
            std::filesystem::create_directories(dumpDir, code);
            std::ofstream out(std::filesystem::path(dumpDir) /
                              (name + "_displace.comp.glsl"));
            out << generated;
        }

        CollectTexturePaths(document, name, resolvedTextures, generator.get(),
                            udimSubstitution, texturePaths);

        hdclaude::GlslCompileOptions options;
        options.moduleName = name + "_displace";
        const hdclaude::GlslCompileResult compiled =
            _compiler.Compile(generated + _displaceKernel, options);
        if (!compiled.ok) {
            if (error) {
                *error = "displacement compilation failed: " + compiled.log;
            }
            if (texturePaths) {
                texturePaths->clear();
            }
            return false;
        }

        material->displaceSpirv = compiled.spirv;
        material->displacementSpace = terminal.space;
        HdClaudeTrace(
            "material %s: displacement compiled, %zu SPIR-V words, offset read "
            "%s, %zu texture(s)",
            name.c_str(), compiled.spirv.size(),
            terminal.space == hdclaude::DisplacementSpace::Tangent
                ? "in the (dPdu, dPdv, N) frame"
                : "along the normal",
            texturePaths != nullptr ? texturePaths->size() : std::size_t(0));
    } catch (const std::exception& exception) {
        if (error) {
            *error = std::string("displacement generation failed: ") +
                     exception.what();
        }
        if (texturePaths) {
            texturePaths->clear();
        }
        return false;
    }
    return true;
}

hdclaude::CompiledMaterial HdClaudeMaterialCompiler::CompileDiffuse(
    const GfVec3f& color, const std::string& name)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!Ready()) {
        return {};
    }

    // Built as a MaterialX document, not as a special case in the renderer.
    // Even the fallback goes through the one shading path, so there is no
    // second code path to keep correct.
    mx::DocumentPtr document = mx::createDocument();
    document->importLibrary(_libraries);

    mx::NodePtr bsdf = document->addNode("oren_nayar_diffuse_bsdf",
                                         "hdclaudeFallbackBsdf", "BSDF");
    if (!bsdf || !bsdf->getNodeDef()) {
        return {};
    }
    bsdf->addInputFromNodeDef("weight")->setValue(1.0f);
    bsdf->addInputFromNodeDef("color")->setValue(
        mx::Color3(color[0], color[1], color[2]));
    bsdf->addInputFromNodeDef("roughness")->setValue(0.0f);

    mx::NodePtr surface = document->addNode("surface", "hdclaudeFallbackSurface",
                                            "surfaceshader");
    surface->addInputFromNodeDef("bsdf")->setConnectedNode(bsdf);
    surface->addInputFromNodeDef("opacity")->setValue(1.0f);

    mx::NodePtr material = document->addNode("surfacematerial",
                                             "hdclaudeFallbackMaterial", "material");
    material->addInputFromNodeDef("surfaceshader")->setConnectedNode(surface);

    std::string error;
    hdclaude::CompiledMaterial compiled = CompileDocument(document, name, &error);
    if (compiled.spirv.empty() && !error.empty()) {
        TF_RUNTIME_ERROR("hdClaude: the fallback material failed to compile: %s",
                         error.c_str());
    }
    return compiled;
}

HdClaudeMaterialCompiler::Result HdClaudeMaterialCompiler::Compile(
    const HdMaterialNetworkMap& networkMap, const SdfPath& path,
    const GfVec3f& fallbackColor)
{
    Result result;
    const std::string name = "hdclaude_" + HdMtlxCreateNameFromPath(path);

    if (!Ready()) {
        result.fallbackReason = "the MaterialX libraries are not available";
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    // Taken here rather than around CompileDocument alone: building the
    // MaterialX document reads the shared library document, which is as
    // unsafe to share across threads as the generator is.
    std::unique_lock<std::mutex> lock(_mutex);

    // HdMaterialNetwork2 is the form hdMtlx consumes, and converting is a
    // public API rather than something to reimplement.
    bool isVolume = false;
    const HdMaterialNetwork2 network =
        HdConvertToHdMaterialNetwork2(networkMap, &isVolume);

    if (isVolume) {
        result.fallbackReason = "volume materials are not implemented";
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    auto terminal = network.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == network.terminals.end()) {
        result.fallbackReason = "the network has no surface terminal";
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    const SdfPath& terminalPath = terminal->second.upstreamNode;
    auto terminalNode = network.nodes.find(terminalPath);
    if (terminalNode == network.nodes.end()) {
        result.fallbackReason = "the surface terminal names a missing node";
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    if (!IsMaterialXSurface(terminalNode->second.nodeTypeId)) {
        // The UsdPreviewSurface case, and anything else non-MaterialX. Reported
        // rather than translated: hdClaude contains no extractor for a named
        // surface model, and inventing one here would be exactly the
        // "approximated material" state the architecture rules out.
        result.fallbackReason =
            "'" + terminalNode->second.nodeTypeId.GetString() +
            "' is not a MaterialX surface; shaded with displayColor instead";
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    // Hoisted out of the try below because the displacement terminal is built
    // from the same translated network: the two terminals are two graphs of
    // one material, and translating twice could translate them differently.
    const HdMaterialNetwork2 translated = TranslateUsdNodeTypes(network);

    mx::DocumentPtr document;
    std::map<std::string, std::string> resolvedTextures;
    try {
        HdMtlxTexturePrimvarData mxHdData;
        const auto translatedTerminal = translated.nodes.find(terminalPath);
        document = HdMtlxCreateMtlxDocumentFromHdNetwork(
            translated, translatedTerminal->second, terminalPath, path,
            _libraries, &mxHdData);
        resolvedTextures = ResolvedTexturePaths(translated, mxHdData);
        ResolvePrimvarReaders(document, name);
        PruneUndeclaredInputs(document, name);
    } catch (const std::exception& error) {
        result.fallbackReason =
            std::string("could not build a MaterialX document: ") + error.what();
    }

    if (!document) {
        if (result.fallbackReason.empty()) {
            result.fallbackReason = "could not build a MaterialX document";
        }
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    std::string error;
    result.material = CompileDocument(document, name, &error,
                                      &result.texturePaths, &resolvedTextures);
    if (result.material.spirv.empty()) {
        // A material that fails to compile names itself, the prim, and the
        // compiler message. It is not silently replaced with something that
        // looks plausible.
        result.fallbackReason = error;
        result.texturePaths.clear();
        TF_RUNTIME_ERROR("hdClaude: material <%s> did not compile: %s",
                         path.GetText(), error.c_str());
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
        return result;
    }

    // --- The displacement terminal ------------------------------------------
    //
    // A second graph of the same material, with a second program: MaterialX's
    // `displacement` output is a terminal beside `surface`, and hdMtlx builds
    // a document from whichever terminal node it is handed. Attempted only
    // after the surface compiled, because the fallback the surface falls back
    // to has no displacement to attach one to.
    //
    // A displacement that fails does not take the material with it. The
    // surface is still the surface as authored, and the honest consequence is
    // an undisplaced mesh with a message saying so -- not a grey stand-in for
    // shading that was perfectly good.
    const auto displacement =
        network.terminals.find(HdMaterialTerminalTokens->displacement);
    if (displacement == network.terminals.end()) {
        // Traced rather than passed over in silence: a network whose
        // displacement terminal never reached Hydra is indistinguishable, from
        // the image, from a material that does not displace.
        HdClaudeTrace("material <%s>: the network has no displacement terminal",
                      path.GetText());
    }
    if (displacement != network.terminals.end()) {
        const SdfPath& displacementPath = displacement->second.upstreamNode;
        const auto displacementNode = translated.nodes.find(displacementPath);
        if (displacementNode == translated.nodes.end()) {
            result.displacementReason =
                "the displacement terminal names a missing node";
        } else {
            try {
                HdMtlxTexturePrimvarData displaceHdData;
                mx::DocumentPtr displaceDocument =
                    HdMtlxCreateMtlxDocumentFromHdNetwork(
                        translated, displacementNode->second, displacementPath,
                        path, _libraries, &displaceHdData);
                if (!displaceDocument) {
                    result.displacementReason =
                        "could not build a MaterialX document for the "
                        "displacement terminal";
                } else {
                    std::map<std::string, std::string> displaceTextures =
                        ResolvedTexturePaths(translated, displaceHdData);
                    ResolvePrimvarReaders(displaceDocument, name);
                    PruneUndeclaredInputs(displaceDocument, name);

                    std::string displaceError;
                    CompileDisplacement(displaceDocument, name,
                                        &result.material, &displaceError,
                                        &result.displacementTexturePaths,
                                        &displaceTextures);
                    result.displacementReason = std::move(displaceError);
                }
            } catch (const std::exception& built) {
                result.displacementReason =
                    std::string(
                        "could not build a MaterialX document for the "
                        "displacement terminal: ") +
                    built.what();
            }
        }

        if (!result.displacementReason.empty()) {
            result.displacementTexturePaths.clear();
            TF_WARN(
                "hdClaude: material <%s> authors a displacement that was not "
                "compiled: %s; the mesh is rendered undisplaced",
                path.GetText(), result.displacementReason.c_str());
        }
    }
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
