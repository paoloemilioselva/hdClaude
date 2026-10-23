"""Zip a built hdClaude for Houdini, to extract into a Houdini user preferences
directory.

The zip holds what `cmake --install` puts in the package prefix -- the plugin,
its shaders and MaterialX tree, UsdRenderers.json and the parameter files --
with the licences of everything compiled into the plugin, a package file that
finds the plugin as $HOUDINI_USER_PREF_DIR/hdClaude, and README.txt:

  hdClaude/
  packages/hdClaude.json
  README.txt

The install is staged with the default component only. The package file this
machine's install writes is its own component (src/hydra/CMakeLists.txt),
because it names this machine's prefix and lives outside it.

The plugin reads Houdini's MaterialX libraries from the Houdini it was built
against, at that path, so the README names that build and that location.

Usage, after `python compile.py houdini-22.0`:

  python scripts/package_houdini.py [--build-dir build/houdini-22.0]
                                    [--output build/dist]
"""

import argparse
import os
import pathlib
import re
import shutil
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
# The build driver's CMake lookup, which enters the Visual Studio developer
# environment when CMake is not on PATH.
import compile as build_driver  # noqa: E402

# Compiled into the plugin, so their licences travel with it. DLSS is not
# here: the Houdini preset does not enable it.
THIRD_PARTY_LICENCES = {
    "glslang.txt": "_deps/glslang-src/LICENSE.txt",
    "volk.txt": "_deps/volk-src/LICENSE.md",
    "VulkanMemoryAllocator.txt": "_deps/vma-src/LICENSE.txt",
    "Vulkan-Headers.txt": "_deps/vulkan_headers-src/LICENSE.md",
}

# What an install that loads and shades has to contain. Each of these is
# otherwise found through a path compiled in on the build machine, so a zip
# missing one would work here and nowhere else.
REQUIRED = [
    "dso/usd_plugins/hdClaude.dll",
    "dso/usd_plugins/plugInfo.json",
    "dso/usd_plugins/hdClaude/resources/plugInfo.json",
    "dso/usd_plugins/hdClaude/resources/shaders/shade.comp.glsl",
    "dso/usd_plugins/hdClaude/resources/mtlx/targets/genglsl_pt.mtlx",
    "UsdRenderers.json",
    "soho/parameters/HdClaudeRendererPlugin_Global.ds",
    "soho/parameters/HdClaudeRendererPlugin_Viewport.ds",
]


def cache_value(cache: pathlib.Path, name: str) -> str:
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith(name + ":"):
            return line.split("=", 1)[1]
    sys.exit(f"{name} is not in {cache}; is this a Houdini build directory?")


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(hdClaude VERSION (\S+)", text)
    if not match:
        sys.exit("No project version in CMakeLists.txt.")
    return match.group(1)


def houdini_build(houdini_root: pathlib.Path) -> str:
    header = houdini_root / "toolkit/include/SYS/SYS_Version.h"
    match = re.search(r'#define SYS_VERSION_FULL "([^"]+)"',
                      header.read_text(encoding="utf-8"))
    if not match:
        sys.exit(f"No SYS_VERSION_FULL in {header}.")
    return match.group(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=pathlib.Path,
                        default=ROOT / "build/houdini-22.0")
    parser.add_argument("--output", type=pathlib.Path,
                        default=ROOT / "build/dist")
    args = parser.parse_args()

    build = args.build_dir.resolve()
    cache = build / "CMakeCache.txt"
    if not cache.exists():
        sys.exit(f"No CMakeCache.txt in {build}; build the Houdini preset first.")
    houdini_root = pathlib.Path(cache_value(cache, "HDCLAUDE_HOUDINI_ROOT"))
    version = project_version()
    houdini = houdini_build(houdini_root)

    with tempfile.TemporaryDirectory() as temporary:
        stage = pathlib.Path(temporary)
        package = stage / "hdClaude"
        build_driver.run(["cmake", "--install", str(build), "--prefix",
                          str(package), "--component", "Unspecified"],
                         build_driver.toolchain_environment(os.environ.copy()))

        missing = [p for p in REQUIRED if not (package / p).is_file()]
        if missing:
            sys.exit("The staged install is missing:\n  " + "\n  ".join(missing))

        licences = package / "licenses"
        licences.mkdir()
        shutil.copyfile(ROOT / "LICENSE", licences / "hdClaude.txt")
        for name, source in THIRD_PARTY_LICENCES.items():
            path = ROOT / source
            if not path.is_file():
                sys.exit(f"Licence not found: {path}")
            shutil.copyfile(path, licences / name)

        (stage / "packages").mkdir()
        template = (ROOT / "resources/houdini/hdClaude.json.in").read_text(
            encoding="utf-8")
        (stage / "packages/hdClaude.json").write_text(
            template.replace("@HDCLAUDE_PACKAGE_ROOT@",
                             "$HOUDINI_USER_PREF_DIR/hdClaude"),
            encoding="utf-8")

        readme = (ROOT / "resources/houdini/README.txt.in").read_text(
            encoding="utf-8")
        for key, value in {"VERSION": version, "HOUDINI_VERSION": houdini,
                           "HOUDINI_ROOT": str(houdini_root)}.items():
            readme = readme.replace(f"@{key}@", value)
        if re.search(r"@[A-Z_]+@", readme):
            sys.exit("README.txt.in has a placeholder this script does not fill.")
        # Notepad is where most people will open it.
        (stage / "README.txt").write_bytes(
            readme.replace("\n", "\r\n").encode("utf-8"))

        args.output.mkdir(parents=True, exist_ok=True)
        archive = args.output / f"hdClaude-{version}-houdini{houdini}-windows-x64.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    out.write(path, path.relative_to(stage).as_posix())
        count = sum(1 for p in stage.rglob("*") if p.is_file())

    print(f"{archive}  ({count} files, {archive.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
