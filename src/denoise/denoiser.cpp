#include "hdclaude/denoise/denoiser.h"

#include "hdclaude/core/environment.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(HDCLAUDE_HAS_OIDN)
#include <OpenImageDenoise/oidn.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace hdclaude {

#if defined(HDCLAUDE_HAS_OIDN)

namespace {

#if defined(_WIN32)
constexpr const char* kLibraryName = "OpenImageDenoise.dll";
#elif defined(__APPLE__)
constexpr const char* kLibraryName = "libOpenImageDenoise.2.dylib";
#else
// The soname, not the unversioned development link: a runtime package carries
// only the soname, and it is the major version the C API is stable within.
constexpr const char* kLibraryName = "libOpenImageDenoise.so.2";
#endif

using Handle = void*;

Handle AlreadyLoaded()
{
#if defined(_WIN32)
    return reinterpret_cast<Handle>(GetModuleHandleA(kLibraryName));
#else
    return dlopen(kLibraryName, RTLD_NOW | RTLD_NOLOAD);
#endif
}

/// Loads by path, its dependencies resolved from its own directory rather than
/// the process's -- which is what lets OpenImageDenoise_core and the device
/// modules beside it be the ones that load.
Handle LoadFile(const std::filesystem::path& file)
{
#if defined(_WIN32)
    return reinterpret_cast<Handle>(LoadLibraryExW(
        file.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
#else
    return dlopen(file.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

Handle LoadByName()
{
#if defined(_WIN32)
    return reinterpret_cast<Handle>(LoadLibraryA(kLibraryName));
#else
    return dlopen(kLibraryName, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* Symbol(Handle library, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

/// The file a loaded library came from, for the description.
std::string LoadedFrom(Handle library, void* anySymbol)
{
#if defined(_WIN32)
    (void)anySymbol;
    wchar_t path[MAX_PATH] = {};
    const DWORD length =
        GetModuleFileNameW(reinterpret_cast<HMODULE>(library), path, MAX_PATH);
    return length > 0 ? std::filesystem::path(path).string() : std::string();
#else
    (void)library;
    Dl_info info{};
    if (anySymbol != nullptr && dladdr(anySymbol, &info) != 0 &&
        info.dli_fname != nullptr) {
        return info.dli_fname;
    }
    return {};
#endif
}

const char* DeviceName(int type)
{
    switch (type) {
        case OIDN_DEVICE_TYPE_CPU: return "CPU";
        case OIDN_DEVICE_TYPE_SYCL: return "SYCL";
        case OIDN_DEVICE_TYPE_CUDA: return "CUDA";
        case OIDN_DEVICE_TYPE_HIP: return "HIP";
        case OIDN_DEVICE_TYPE_METAL: return "Metal";
        default: return "unknown";
    }
}

}  // namespace

/// The C API, resolved by name. Every entry is one the 2.x API has had since
/// 2.0, so a host's older 2.x -- Houdini 22.0 ships 2.3.3 -- serves as well as
/// the version the headers come from.
struct Denoiser::Library {
    Handle handle = nullptr;
    bool owned = false;
    OIDNDevice device = nullptr;

#define HDCLAUDE_OIDN_FUNCTION(name) decltype(&::name) name = nullptr
    HDCLAUDE_OIDN_FUNCTION(oidnNewDevice);
    HDCLAUDE_OIDN_FUNCTION(oidnCommitDevice);
    HDCLAUDE_OIDN_FUNCTION(oidnReleaseDevice);
    HDCLAUDE_OIDN_FUNCTION(oidnGetDeviceError);
    HDCLAUDE_OIDN_FUNCTION(oidnGetDeviceInt);
    HDCLAUDE_OIDN_FUNCTION(oidnNewBuffer);
    HDCLAUDE_OIDN_FUNCTION(oidnWriteBuffer);
    HDCLAUDE_OIDN_FUNCTION(oidnReadBuffer);
    HDCLAUDE_OIDN_FUNCTION(oidnReleaseBuffer);
    HDCLAUDE_OIDN_FUNCTION(oidnNewFilter);
    HDCLAUDE_OIDN_FUNCTION(oidnSetFilterImage);
    HDCLAUDE_OIDN_FUNCTION(oidnSetFilterBool);
    HDCLAUDE_OIDN_FUNCTION(oidnSetFilterInt);
    HDCLAUDE_OIDN_FUNCTION(oidnCommitFilter);
    HDCLAUDE_OIDN_FUNCTION(oidnExecuteFilter);
    HDCLAUDE_OIDN_FUNCTION(oidnReleaseFilter);
#undef HDCLAUDE_OIDN_FUNCTION

    /// Resolves every entry; the name of the first missing one, or null.
    const char* Resolve()
    {
#define HDCLAUDE_OIDN_RESOLVE(name)                                          \
    name = reinterpret_cast<decltype(name)>(Symbol(handle, #name));          \
    if (name == nullptr) {                                                   \
        return #name;                                                        \
    }
        HDCLAUDE_OIDN_RESOLVE(oidnNewDevice);
        HDCLAUDE_OIDN_RESOLVE(oidnCommitDevice);
        HDCLAUDE_OIDN_RESOLVE(oidnReleaseDevice);
        HDCLAUDE_OIDN_RESOLVE(oidnGetDeviceError);
        HDCLAUDE_OIDN_RESOLVE(oidnGetDeviceInt);
        HDCLAUDE_OIDN_RESOLVE(oidnNewBuffer);
        HDCLAUDE_OIDN_RESOLVE(oidnWriteBuffer);
        HDCLAUDE_OIDN_RESOLVE(oidnReadBuffer);
        HDCLAUDE_OIDN_RESOLVE(oidnReleaseBuffer);
        HDCLAUDE_OIDN_RESOLVE(oidnNewFilter);
        HDCLAUDE_OIDN_RESOLVE(oidnSetFilterImage);
        HDCLAUDE_OIDN_RESOLVE(oidnSetFilterBool);
        HDCLAUDE_OIDN_RESOLVE(oidnSetFilterInt);
        HDCLAUDE_OIDN_RESOLVE(oidnCommitFilter);
        HDCLAUDE_OIDN_RESOLVE(oidnExecuteFilter);
        HDCLAUDE_OIDN_RESOLVE(oidnReleaseFilter);
#undef HDCLAUDE_OIDN_RESOLVE
        return nullptr;
    }

    /// OIDN's pending error, cleared by reading it; empty when there is none.
    std::string TakeError()
    {
        const char* message = nullptr;
        const OIDNError code = oidnGetDeviceError(device, &message);
        if (code == OIDN_ERROR_NONE) {
            return {};
        }
        return message != nullptr && *message != 0
                   ? std::string(message)
                   : "error " + std::to_string(static_cast<int>(code));
    }

    ~Library()
    {
        if (device != nullptr && oidnReleaseDevice != nullptr) {
            oidnReleaseDevice(device);
        }
        // A library this object did not load stays loaded, and one it did is
        // left loaded too: OIDN's device modules keep driver state -- a CUDA
        // context -- that is not safe to tear down under a host still running.
        (void)owned;
    }
};

Denoiser::Denoiser(const std::string& runtimeDirectory, DenoiserDevice device)
    : _library(std::make_unique<Library>())
{
    Library& library = *_library;
    std::string where;

    if ((library.handle = AlreadyLoaded()) != nullptr) {
        where = "already loaded by the host";
    }
    if (library.handle == nullptr) {
        const std::string overrideDirectory = EnvironmentValue("HDCLAUDE_OIDN_DIR");
        if (!overrideDirectory.empty()) {
            const std::filesystem::path file =
                std::filesystem::path(overrideDirectory) / kLibraryName;
            library.handle = LoadFile(file);
            if (library.handle == nullptr) {
                _unavailable = "HDCLAUDE_OIDN_DIR names " + file.string() +
                               ", which could not be loaded";
                return;
            }
            where = "HDCLAUDE_OIDN_DIR";
            library.owned = true;
        }
    }
    if (library.handle == nullptr && !runtimeDirectory.empty()) {
        const std::filesystem::path file =
            std::filesystem::path(runtimeDirectory) / kLibraryName;
        std::error_code code;
        if (std::filesystem::exists(file, code)) {
            library.handle = LoadFile(file);
            if (library.handle == nullptr) {
                _unavailable = file.string() + " is there but could not be loaded";
                return;
            }
            where = "the copy installed with hdClaude";
            library.owned = true;
        }
    }
    if (library.handle == nullptr) {
        library.handle = LoadByName();
        if (library.handle == nullptr) {
            _unavailable = std::string("no ") + kLibraryName +
                           " was found: not in the host, not beside the "
                           "plugin, and not on the library search path";
            return;
        }
        where = "the library search path";
        library.owned = true;
    }

    if (const char* missing = library.Resolve()) {
        _unavailable = std::string("the OIDN library loaded has no ") + missing +
                       ", so it is not a 2.x release";
        return;
    }

    OIDNDeviceType type = OIDN_DEVICE_TYPE_DEFAULT;
    switch (device) {
        case DenoiserDevice::Default: type = OIDN_DEVICE_TYPE_DEFAULT; break;
        case DenoiserDevice::Cpu: type = OIDN_DEVICE_TYPE_CPU; break;
        case DenoiserDevice::Cuda: type = OIDN_DEVICE_TYPE_CUDA; break;
        case DenoiserDevice::Hip: type = OIDN_DEVICE_TYPE_HIP; break;
        case DenoiserDevice::Sycl: type = OIDN_DEVICE_TYPE_SYCL; break;
    }
    library.device = library.oidnNewDevice(type);
    if (library.device == nullptr) {
        _unavailable = std::string("OIDN could not create a ") +
                       (device == DenoiserDevice::Default ? "device"
                                                          : DeviceName(type)) +
                       std::string(" device on this machine");
        return;
    }
    library.oidnCommitDevice(library.device);
    const std::string error = library.TakeError();
    if (!error.empty()) {
        _unavailable = "OIDN could not start its device: " + error;
        library.oidnReleaseDevice(library.device);
        library.device = nullptr;
        return;
    }

    const int version = library.oidnGetDeviceInt(library.device, "version");
    const int deviceType = library.oidnGetDeviceInt(library.device, "type");
    char described[96];
    std::snprintf(described, sizeof(described), "Open Image Denoise %d.%d.%d, %s device",
                  version / 10000, (version / 100) % 100, version % 100,
                  DeviceName(deviceType));
    _description = described;
    const std::string file =
        LoadedFrom(library.handle, reinterpret_cast<void*>(library.oidnNewDevice));
    _description += ", " + where;
    if (!file.empty()) {
        _description += " (" + file + ")";
    }
}

Denoiser::~Denoiser() = default;

bool Denoiser::Available() const
{
    return _library && _library->device != nullptr;
}

bool Denoiser::Denoise(const DenoiseInputs& inputs, DenoiserQuality quality,
                       std::vector<float>* output, std::string* error)
{
    std::string ignored;
    std::string& message = error != nullptr ? *error : ignored;
    if (!Available()) {
        message = _unavailable;
        return false;
    }
    if (inputs.width == 0 || inputs.height == 0 || inputs.color == nullptr ||
        output == nullptr) {
        message = "nothing to denoise";
        return false;
    }
    Library& library = *_library;
    const std::size_t pixels =
        static_cast<std::size_t>(inputs.width) * inputs.height;
    const std::size_t bytes = pixels * 3 * sizeof(float);
    const bool withAlbedo = inputs.albedo != nullptr;
    const bool withNormal = withAlbedo && inputs.normal != nullptr;

    // Three floats a pixel, packed: the colour drops its alpha here, so the
    // image OIDN reads is the layout every device supports.
    std::vector<float> packed(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        packed[i * 3 + 0] = inputs.color[i * 4 + 0];
        packed[i * 3 + 1] = inputs.color[i * 4 + 1];
        packed[i * 3 + 2] = inputs.color[i * 4 + 2];
    }

    struct Buffers {
        Library& library;
        OIDNBuffer color = nullptr, albedo = nullptr, normal = nullptr,
                   result = nullptr;
        OIDNFilter image = nullptr, albedoFilter = nullptr, normalFilter = nullptr;
        ~Buffers()
        {
            for (OIDNFilter filter : {image, albedoFilter, normalFilter}) {
                if (filter != nullptr) {
                    library.oidnReleaseFilter(filter);
                }
            }
            for (OIDNBuffer buffer : {color, albedo, normal, result}) {
                if (buffer != nullptr) {
                    library.oidnReleaseBuffer(buffer);
                }
            }
        }
    } buffers{library};

    // OIDN reports a failure by recording it on the device, so each step is
    // followed by reading it -- once, since reading clears it.
    const auto fail = [&](const char* step) {
        const std::string reason = library.TakeError();
        message = std::string("OIDN failed to ") + step +
                  (reason.empty() ? std::string() : ": " + reason);
        return false;
    };
    const auto failed = [&](const char* step) {
        const std::string reason = library.TakeError();
        if (reason.empty()) {
            return false;
        }
        message = std::string("OIDN failed to ") + step + ": " + reason;
        return true;
    };

    buffers.color = library.oidnNewBuffer(library.device, bytes);
    buffers.result = library.oidnNewBuffer(library.device, bytes);
    if (withAlbedo) {
        buffers.albedo = library.oidnNewBuffer(library.device, bytes);
    }
    if (withNormal) {
        buffers.normal = library.oidnNewBuffer(library.device, bytes);
    }
    if (buffers.color == nullptr || buffers.result == nullptr ||
        (withAlbedo && buffers.albedo == nullptr) ||
        (withNormal && buffers.normal == nullptr)) {
        return fail("allocate its buffers");
    }
    library.oidnWriteBuffer(buffers.color, 0, bytes, packed.data());
    if (withAlbedo) {
        library.oidnWriteBuffer(buffers.albedo, 0, bytes, inputs.albedo);
    }
    if (withNormal) {
        library.oidnWriteBuffer(buffers.normal, 0, bytes, inputs.normal);
    }

    const int oidnQuality = quality == DenoiserQuality::Fast       ? OIDN_QUALITY_FAST
                            : quality == DenoiserQuality::Balanced ? OIDN_QUALITY_BALANCED
                                                                   : OIDN_QUALITY_HIGH;
    const auto image = [&](OIDNFilter filter, const char* name, OIDNBuffer buffer) {
        library.oidnSetFilterImage(filter, name, buffer, OIDN_FORMAT_FLOAT3,
                                   inputs.width, inputs.height, 0, 0, 0);
    };

    // The guides first, each through the same filter in place: an average of
    // a few samples of an albedo or a normal is itself noisy wherever the
    // pixel sees more than one surface -- an edge, depth of field, a rough
    // transmission -- and a noisy guide hands that noise to the image.
    if (withAlbedo) {
        buffers.albedoFilter = library.oidnNewFilter(library.device, "RT");
        if (buffers.albedoFilter == nullptr) {
            return fail("create the albedo filter");
        }
        image(buffers.albedoFilter, "albedo", buffers.albedo);
        image(buffers.albedoFilter, "output", buffers.albedo);
        library.oidnSetFilterInt(buffers.albedoFilter, "quality", oidnQuality);
        library.oidnCommitFilter(buffers.albedoFilter);
        library.oidnExecuteFilter(buffers.albedoFilter);
        if (failed("clean the albedo")) {
            return false;
        }
    }
    if (withNormal) {
        buffers.normalFilter = library.oidnNewFilter(library.device, "RT");
        if (buffers.normalFilter == nullptr) {
            return fail("create the normal filter");
        }
        image(buffers.normalFilter, "normal", buffers.normal);
        image(buffers.normalFilter, "output", buffers.normal);
        library.oidnSetFilterInt(buffers.normalFilter, "quality", oidnQuality);
        library.oidnCommitFilter(buffers.normalFilter);
        library.oidnExecuteFilter(buffers.normalFilter);
        if (failed("clean the normal")) {
            return false;
        }
    }

    buffers.image = library.oidnNewFilter(library.device, "RT");
    if (buffers.image == nullptr) {
        return fail("create the filter");
    }
    image(buffers.image, "color", buffers.color);
    if (withAlbedo) {
        image(buffers.image, "albedo", buffers.albedo);
    }
    if (withNormal) {
        image(buffers.image, "normal", buffers.normal);
    }
    image(buffers.image, "output", buffers.result);
    library.oidnSetFilterBool(buffers.image, "hdr", true);
    library.oidnSetFilterBool(buffers.image, "cleanAux", withAlbedo);
    library.oidnSetFilterInt(buffers.image, "quality", oidnQuality);
    library.oidnCommitFilter(buffers.image);
    library.oidnExecuteFilter(buffers.image);
    if (failed("denoise the image")) {
        return false;
    }

    library.oidnReadBuffer(buffers.result, 0, bytes, packed.data());
    if (failed("read the result back")) {
        return false;
    }
    output->resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        (*output)[i * 4 + 0] = packed[i * 3 + 0];
        (*output)[i * 4 + 1] = packed[i * 3 + 1];
        (*output)[i * 4 + 2] = packed[i * 3 + 2];
        (*output)[i * 4 + 3] = inputs.color[i * 4 + 3];
    }
    return true;
}

bool DenoiserCompiledIn() { return true; }

#else   // HDCLAUDE_HAS_OIDN

struct Denoiser::Library {};

Denoiser::Denoiser(const std::string&, DenoiserDevice)
    : _unavailable("this build has no Open Image Denoise headers")
{
}

Denoiser::~Denoiser() = default;

bool Denoiser::Available() const { return false; }

bool Denoiser::Denoise(const DenoiseInputs&, DenoiserQuality, std::vector<float>*,
                       std::string* error)
{
    if (error != nullptr) {
        *error = _unavailable;
    }
    return false;
}

bool DenoiserCompiledIn() { return false; }

#endif   // HDCLAUDE_HAS_OIDN

const std::string& Denoiser::Unavailable() const { return _unavailable; }

const std::string& Denoiser::Description() const { return _description; }

}  // namespace hdclaude
