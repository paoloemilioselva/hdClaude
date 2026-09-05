#include "material_compiler.h"

#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/HwShaderGenerator.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"

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

/// Names the texture-sampling uniforms a generated shader declares.
///
/// hdClaude does not bind texture descriptors yet, and a compute pipeline whose
/// shader samples an unbound descriptor does not draw something wrong -- it
/// faults the device. So a material that needs textures is refused here, by
/// name, rather than compiled into a pipeline that takes the GPU down on its
/// first dispatch.
///
/// Checked against the generated shader's own uniform blocks rather than
/// against the Hydra network, because what matters is what the generated code
/// actually reads.
std::vector<std::string> TextureUniforms(const mx::ShaderPtr& shader)
{
    std::vector<std::string> names;
    const mx::ShaderStage& stage = shader->getStage(mx::Stage::PIXEL);
    for (const auto& [blockName, block] : stage.getUniformBlocks()) {
        if (!block) {
            continue;
        }
        for (std::size_t i = 0; i < block->size(); ++i) {
            const mx::ShaderPort* port = (*block)[i];
            if (port && port->getType() == mx::Type::FILENAME) {
                names.push_back(port->getVariable());
            }
        }
    }
    return names;
}

/// Join a handful of names for a diagnostic.
std::string JoinNames(const std::vector<std::string>& names)
{
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += names[i];
    }
    return joined;
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
    bool* unsupported)
{
    if (unsupported) {
        *unsupported = false;
    }
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

        mx::ShaderPtr shader =
            generator->generate(name, renderable.front(), context);
        if (!shader) {
            if (error) *error = "MaterialX generated no shader";
            return result;
        }

        const std::vector<std::string> textures = TextureUniforms(shader);
        if (!textures.empty()) {
            if (unsupported) {
                *unsupported = true;
            }
            if (error) {
                *error = "the material samples textures (" +
                         JoinNames(textures) +
                         "), which hdClaude does not bind yet; shaded with "
                         "displayColor instead";
            }
            return result;
        }

        const std::string source =
            shader->getSourceCode(mx::Stage::PIXEL) + _shadeKernel;

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
    try {
        HdMtlxTexturePrimvarData mxHdData;
        document = HdMtlxCreateMtlxDocumentFromHdNetwork(
            network, terminalNode->second, terminalPath, path, _libraries,
            &mxHdData);
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
    bool unsupported = false;
    result.material = CompileDocument(document, name, &error, &unsupported);
    if (result.material.spirv.empty()) {
        // A material that fails to compile names itself, the prim, and the
        // compiler message. It is not silently replaced with something that
        // looks plausible. A material hdClaude simply cannot run yet is
        // reported by the prim as a fallback instead -- it is a known gap, not
        // a defect, and raising it as an error here would bury the real ones.
        result.fallbackReason = error;
        if (!unsupported) {
            TF_RUNTIME_ERROR("hdClaude: material <%s> did not compile: %s",
                             path.GetText(), error.c_str());
        }
        lock.unlock();
        result.material = CompileDiffuse(fallbackColor, name + "_fallback");
    }
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
