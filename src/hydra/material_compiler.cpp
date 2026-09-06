#include "material_compiler.h"

#include "trace.h"

#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/HwShaderGenerator.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include "pxr/base/gf/vec3f.h"
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
bool IsMaterialXSurface(const TfToken& nodeType)
{
    static const TfToken kNdPrefix("ND_");
    const std::string& name = nodeType.GetString();
    return name.rfind(kNdPrefix.GetString(), 0) == 0 ||
           name == "standard_surface" || name == "open_pbr_surface" ||
           name == "surface" || name == "gltf_pbr" ||
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

std::string ResolveTexturePath(const mx::DocumentPtr& document,
                               const std::string& uniformName)
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
                return input->getResolvedValueString();
            }
        }
    }
    return std::string();
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

}  // namespace

HdClaudeMaterialCompiler::HdClaudeMaterialCompiler(std::string shadeKernel)
    : _shadeKernel(std::move(shadeKernel))
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
    std::vector<std::string>* texturePaths,
    const std::map<std::string, std::string>* resolvedTextures)
{
    // Caller holds _mutex: this touches the shared library document, the
    // generator, and glslang, none of which are thread-safe.
    hdclaude::CompiledMaterial result;
    result.debugName = name;

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
        if (const std::string dumpDir = TfGetenv("HDCLAUDE_DUMP_SHADERS");
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
        if (const std::string dumpDir = TfGetenv("HDCLAUDE_DUMP_SHADERS");
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
        if (texturePaths) {
            if (auto* ptGenerator =
                    dynamic_cast<hdclaude::PathTracerShaderGenerator*>(
                        generator.get())) {
                for (const std::string& uniform : ptGenerator->TextureOrder()) {
                    // The network's resolved path first; the document's
                    // authored one only when the network had nothing, which
                    // happens for a material hdClaude built itself.
                    std::string path;
                    if (resolvedTextures) {
                        const auto found = resolvedTextures->find(uniform);
                        if (found != resolvedTextures->end()) {
                            path = found->second;
                        }
                    }
                    if (path.empty()) {
                        path = ResolveTexturePath(document, uniform);
                    }
                    if (path.empty()) {
                        // An image node with no file at all. That is legal and
                        // common -- an asset authors the node and leaves the
                        // file to a stronger opinion that never arrives -- and
                        // MaterialX says such a node returns its `default`.
                        // The pool turns the empty path into that value, so
                        // this is worth tracing and not worth warning about;
                        // a file that is authored and cannot be read is the
                        // case that deserves to be loud.
                        HdClaudeTrace(
                            "material %s samples '%s', which no asset path "
                            "backs; it reads the image node's default",
                            name.c_str(), uniform.c_str());
                    }
                    texturePaths->push_back(path);
                }
            }
        }

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

    mx::DocumentPtr document;
    std::map<std::string, std::string> resolvedTextures;
    try {
        HdMtlxTexturePrimvarData mxHdData;
        document = HdMtlxCreateMtlxDocumentFromHdNetwork(
            network, terminalNode->second, terminalPath, path, _libraries,
            &mxHdData);
        resolvedTextures = ResolvedTexturePaths(network, mxHdData);
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
    }
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
