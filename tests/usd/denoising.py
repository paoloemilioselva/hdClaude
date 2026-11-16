"""The denoiser, through the render path, against a converged reference.

The gallery's Cornell box at 160 pixels: a reference at 2048 samples, and the
same frame at 16 samples raw and denoised. What is asserted:

  - Denoised, 16 samples sit at least four times nearer the reference than
    raw, by rms over the frame -- roughly what sixteen times the samples would
    buy. Measured at commit: 12 times nearer at 512 pixels.
  - It keeps the light: within 2% of the reference's. A denoiser removes
    fireflies, so on a scene that has them it is below the converged mean; the
    Cornell box has few, and kept 99.5% at commit.
  - Previews follow the schedule. With Denoise from sample 4 and four samples
    a frame, the frames end at 4, 8, 12 and 16 samples, and 4, 8 and the
    finished 16 are denoised: three. With 0, the finished frame alone: one.
  - An interactive render is not denoised, and says why.

Usage: python denoising.py <output directory>
Runs in the OpenUSD environment, and tests the plugin HDCLAUDE_PLUGIN_DIR
names -- the build tree's, under ctest -- or else what compile.py last
installed.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]
STAGE = os.path.join(ROOT, "gallery", "cornell_box.usda")


def render(out, name, samples, denoiser="off", start=16, extra=None):
    image = os.path.join(out, f"{name}.0001.exr")
    stats = os.path.join(out, f"{name}.stats")
    for path in (image, stats):
        if os.path.exists(path):
            os.remove(path)
    environment = dict(os.environ)
    environment.update({
        "HDCLAUDE_SAMPLES_PER_PIXEL": str(samples),
        "HDCLAUDE_SAMPLES_PER_FRAME": "4",
        "HDCLAUDE_RECONSTRUCTION": "off",
        "HDCLAUDE_DENOISER": denoiser,
        "HDCLAUDE_DENOISER_START": str(start),
        "HDCLAUDE_STATS_REPORT": stats,
    })
    environment.update(extra or {})
    result = subprocess.run(
        [*RENDER, "--camera", "camera", "--frames", "1", "--imageWidth", "160",
         STAGE, image.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    output = result.stdout + result.stderr
    if result.returncode != 0 or not os.path.exists(image):
        print(output[-3000:])
        raise SystemExit(f"FAIL: could not render {name}")
    values = {}
    with open(stats, encoding="utf-8") as report:
        for line in report:
            key, _, value = line.strip().partition(" ")
            try:
                values[key] = float(value)
            except ValueError:
                pass
    return image, values, output


def diff(*arguments):
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, *arguments], capture_output=True, text=True)
    return (result.stdout + result.stderr).strip()


def rms(reference, image):
    report = diff(reference, image)
    for part in report.split(","):
        part = part.strip()
        if part.startswith("rms "):
            return float(part.split()[1])
    raise SystemExit(f"FAIL: no rms in {report}")


def kept(reference, image):
    for line in diff("--energy", reference, image).splitlines():
        if "keeps" in line:
            return float(line.split("keeps")[1].split("%")[0])
    raise SystemExit("FAIL: no energy report")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    failures = 0

    reference, _, _ = render(out, "reference", 2048)
    raw, _, _ = render(out, "raw16", 16)
    denoised, stats, log = render(out, "denoised16", 16, "oidn", start=4)

    if "denoising with Open Image Denoise" not in log:
        print(log[-3000:])
        print("FAIL: the render did not say the denoiser ran")
        return 1
    for line in log.splitlines():
        if "denoising with" in line:
            print("  " + line.split("hdClaude: ")[-1])

    raw_rms, denoised_rms = rms(reference, raw), rms(reference, denoised)
    light = kept(reference, denoised)
    print(f"  rms against the reference: raw {raw_rms:.5f}, denoised "
          f"{denoised_rms:.5f} ({raw_rms / max(denoised_rms, 1e-12):.1f}x); "
          f"denoised keeps {light:.2f}% of the light")
    if not denoised_rms * 4.0 < raw_rms:
        print("FAIL: denoised 16 samples are not four times nearer the reference")
        failures += 1
    if abs(light - 100.0) > 2.0:
        print("FAIL: the denoised frame does not keep the reference's light")
        failures += 1

    frames = int(stats.get("denoisedFrames", -1))
    print(f"  from sample 4: {frames} denoised frames, "
          f"{stats.get('denoiseMilliseconds', 0):.1f} ms")
    if frames != 3:
        print("FAIL: expected previews at 4 and 8 samples and the finished 16")
        failures += 1
    _, stats, _ = render(out, "finished16", 16, "oidn", start=0)
    if int(stats.get("denoisedFrames", -1)) != 1:
        print("FAIL: with Denoise from sample 0 only the finished frame is "
              f"denoised; {stats.get('denoisedFrames')} were")
        failures += 1

    _, stats, log = render(out, "interactive", 8, "oidn",
                           extra={"HDCLAUDE_RECONSTRUCTION": "dlaa"})
    if int(stats.get("denoisedFrames", -1)) != 0 or \
            "the denoiser runs on a reference render" not in log:
        print("FAIL: an interactive render was denoised, or did not say why not")
        failures += 1
    else:
        print("  interactive: not denoised, and said so")

    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("denoising: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
