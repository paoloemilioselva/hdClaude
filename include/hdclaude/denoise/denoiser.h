// The final-frame denoiser: Intel Open Image Denoise, loaded at run time.
//
// A path-traced image converges as one over the square root of its samples, so
// halving its noise costs four times the render. A denoiser trades that for a
// learned estimate of the converged image from the noisy one, guided by the
// albedo and normal of what each pixel sees: what is texture and edge there is
// kept, and what is noise is removed. It is an estimate. The film it is given
// is untouched, and the estimate is written only where it was asked for
// (docs/denoising.md).
//
// Nothing links OIDN. The library is found and loaded when a Denoiser is
// constructed, its C API resolved by name, so the plugin loads on a machine
// without it and a Denoiser then answers Available() false and says why. Which
// copy is loaded, in order:
//
//   1. One already in the process. A host that ships OIDN -- Houdini does, and
//      Karma and the compositor load it -- has its copy used, because a second
//      library of the same name in one process resolves its dependencies
//      against the first and crashes in whichever half disagrees.
//   2. `HDCLAUDE_OIDN_DIR`, a directory holding the library, to put a
//      different OIDN in front of the renderer.
//   3. `runtimeDirectory`, the copy a standalone build installs beside the
//      plugin.
//   4. The platform's own search for the bare name, which inside Houdini finds
//      the one in $HFS/bin.
//
// Pure CPU code with no Vulkan and no OpenUSD; the device OIDN picks may be a
// GPU, which it drives itself, through CUDA, HIP or SYCL.

#ifndef HDCLAUDE_DENOISE_DENOISER_H
#define HDCLAUDE_DENOISE_DENOISER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdclaude {

/// OIDN's quality modes: `High` is the one meant for a final frame, `Fast`
/// for an interactive preview.
enum class DenoiserQuality : std::uint8_t { High, Balanced, Fast };

/// Which device OIDN runs on. `Default` lets OIDN choose, which is the fastest
/// it supports: a GPU through CUDA, HIP or SYCL, else the CPU.
enum class DenoiserDevice : std::uint8_t { Default, Cpu, Cuda, Hip, Sycl };

/// One image and its guides, each row-major in the same row order, all the
/// same extent.
struct DenoiseInputs {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Linear HDR radiance, four floats a pixel; the fourth is ignored.
    const float* color = nullptr;
    /// The albedo of what each pixel sees, three floats a pixel in [0, 1],
    /// averaged over the same samples as the colour. Optional.
    const float* albedo = nullptr;
    /// The shading normal, three floats a pixel, averaged likewise. Optional,
    /// and used only with an albedo: OIDN has no normal-only mode.
    const float* normal = nullptr;
};

class Denoiser {
  public:
    /// Load OIDN and create a device. Never throws: a library that cannot be
    /// found, or a device that cannot be made, leaves the denoiser unavailable
    /// with the reason.
    Denoiser(const std::string& runtimeDirectory, DenoiserDevice device);
    ~Denoiser();

    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    bool Available() const;
    /// Why it is not available; empty when it is.
    const std::string& Unavailable() const;
    /// What runs: the version, the device, and the file it was loaded from.
    const std::string& Description() const;

    /// Denoise `inputs` into `output`, RGBA with the colour's alpha. With an
    /// albedo, and a normal, they are cleaned by OIDN first and the image then
    /// denoised against the clean guides -- OIDN's own prefiltering, which is
    /// what keeps the noise in an averaged guide out of the result. False with
    /// `error` set when OIDN reports a failure; `output` is then unchanged.
    bool Denoise(const DenoiseInputs& inputs, DenoiserQuality quality,
                 std::vector<float>* output, std::string* error);

  private:
    struct Library;
    std::unique_ptr<Library> _library;
    std::string _unavailable;
    std::string _description;
};

/// Whether this build has the OIDN headers it needs to load the library at all.
bool DenoiserCompiledIn();

}  // namespace hdclaude

#endif  // HDCLAUDE_DENOISE_DENOISER_H
