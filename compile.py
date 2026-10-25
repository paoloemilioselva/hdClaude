#!/usr/bin/env python3
"""hdClaude - configure, build, and install the delegate.

Usage:
  python compile.py              build the full delegate (preset "dev")
  python compile.py core-only    build only the dependency-free core and its tests
  python compile.py dev-validation  the full delegate, building the validation layer
  python compile.py dev-dlss     build with the optional NVIDIA DLSS backend (Windows)
  python compile.py houdini-22.0 build against the USD inside Houdini 22.0, and
                                 install it as a Houdini package
  python compile.py clean        remove the build tree, then build "dev"

The one build driver on every platform; compile.bat forwards to it. The build
steps live here once, so Windows and Linux cannot drift apart. What differs is
the environment, which comes from setup_usd_env.bat on Windows and
setup_usd_env.sh elsewhere -- those stay shell scripts because they have to be
sourced into an interactive shell for usdview and usdrecord, which a Python
process cannot do for its parent.

Third-party dependencies are fetched by CMake into the gitignored _deps tree on
the first configure. Nothing needs to be installed by hand; see
cmake/Dependencies.cmake for the pinned versions.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == "nt"
MARKER = "__HDCLAUDE_ENVIRONMENT__"

# Entered only when the tools are not already on PATH, so running from a
# developer prompt stays fast.
VSDEVCMD = (r"C:\Program Files\Microsoft Visual Studio\2022\Community"
            r"\Common7\Tools\VsDevCmd.bat")


def log(message):
    print(f"[hdClaude] {message}", flush=True)


def capture_environment(command, environment):
    """Runs a shell command that prints MARKER and then its environment.

    Whatever the command prints before the marker is its own output -- the
    reason a setup script failed -- and is passed through unchanged.
    """
    # cmd writes its own output in the console's OEM code page, not UTF-8.
    result = subprocess.run(command, env=environment, capture_output=True,
                            encoding="oem" if WINDOWS else "utf-8",
                            errors="replace")
    before, found, after = result.stdout.partition(MARKER)
    sys.stdout.write(before.strip("\r\n\0") + ("\n" if before.strip() else ""))
    sys.stderr.write(result.stderr)
    if result.returncode != 0 or not found:
        raise SystemExit(result.returncode or 1)

    separator = "\n" if WINDOWS else "\0"
    captured = {}
    for entry in after.split(separator):
        name, equals, value = entry.partition("=")
        # cmd lists a few per-drive variables named "=C:"; skip them.
        if equals and name:
            # Windows names are case-insensitive and cmd prints "Path"; upper
            # case them as os.environ does, so lookups agree.
            captured[name.upper() if WINDOWS else name] = value
    return captured


def cmd(command):
    """A cmd.exe command line, passed as a string so Python does not re-quote it.

    /s strips only the outer pair of quotes, which keeps quoted paths inside the
    command intact.
    """
    return f'cmd /d /s /c "{command}"'


def usd_environment():
    if WINDOWS:
        script = os.path.join(ROOT, "setup_usd_env.bat")
        return capture_environment(cmd(f'call "{script}" && echo {MARKER}&& set'),
                                   os.environ.copy())
    script = os.path.join(ROOT, "setup_usd_env.sh")
    command = f'. "$1" && printf "{MARKER}\\0" && env -0'
    return capture_environment(["bash", "-c", command, "bash", script],
                               os.environ.copy())


def toolchain_environment(environment):
    """Finds CMake, Ninja, and on Windows MSVC, entering VsDevCmd if needed."""
    path = environment.get("PATH", "")
    tools = ["cmake", "ninja"] + (["cl"] if WINDOWS else [])
    missing = [tool for tool in tools if shutil.which(tool, path=path) is None]
    if not missing:
        return environment

    if not WINDOWS:
        log(f"Not found on PATH: {', '.join(missing)}.")
        log("           Install CMake 3.25 or newer, Ninja, and GCC or Clang.")
        log("           See docs/building.md.")
        raise SystemExit(1)

    if not os.path.exists(VSDEVCMD):
        log("MSVC, CMake, and Ninja were not found, and the Visual Studio")
        log("           2022 developer environment is unavailable at:")
        log(f"           {VSDEVCMD}")
        log("           See docs/building.md.")
        raise SystemExit(1)
    return capture_environment(
        cmd(f'call "{VSDEVCMD}" -arch=x64 -host_arch=x64 >NUL '
            f'&& echo {MARKER}&& set'),
        environment)


def run(arguments, environment):
    # Resolved against the environment being passed: on Windows, CreateProcess
    # searches this process's PATH, not the child's.
    program = shutil.which(arguments[0], path=environment.get("PATH", ""))
    if program is None:
        log(f"{arguments[0]} was not found on the build environment's PATH.")
        raise SystemExit(1)
    result = subprocess.run([program, *arguments[1:]], env=environment)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def main(arguments):
    if len(arguments) > 1 or (arguments and arguments[0] in ("-h", "--help")):
        print(__doc__.strip())
        return 0 if arguments and arguments[0] in ("-h", "--help") else 2

    preset = arguments[0] if arguments else "dev"
    # A Houdini preset finds Houdini's own USD (cmake/HoudiniUSD.cmake) and
    # installs into the Houdini user directory, so the standalone OpenUSD
    # environment is neither needed nor passed on.
    houdini = preset.lower().startswith("houdini-")
    # Nor for core-only, which builds the dependency-free core and its tests:
    # the OpenUSD setup stops when there is no OpenUSD, or when the Python is
    # not the one it was built against, and neither is anything core-only
    # uses.
    core_only = preset.lower() == "core-only"
    if houdini or core_only:
        environment = os.environ.copy()
    else:
        environment = usd_environment()

    if preset.lower() == "clean":
        log("Removing build tree.")
        shutil.rmtree(os.path.join(ROOT, "build"), ignore_errors=True)
        preset = "dev"

    environment = toolchain_environment(environment)

    log(f'Configuring preset "{preset}"...')
    if houdini or core_only:
        run(["cmake", "--preset", preset], environment)
    else:
        # Passed even when empty, so unsetting MATERIALX_ROOT reaches the cache
        # too.
        run(["cmake", "--preset", preset,
             f"-DHDCLAUDE_OPENUSD_ROOT={environment['USDROOT']}",
             f"-DHDCLAUDE_MATERIALX_ROOT={environment.get('MATERIALX_ROOT', '')}",
             f"-DCMAKE_INSTALL_PREFIX={environment['USDEXTRA']}"], environment)

    log("Building...")
    run(["cmake", "--build", "--preset", preset], environment)

    # The core-only preset builds no installable artifact; it exists to run the
    # host-independent tests.
    if preset.lower() != "core-only":
        log("Installing to "
            f"{'the Houdini user directory' if houdini else environment['USDEXTRA']}...")
        run(["cmake", "--build", os.path.join(ROOT, "build", preset),
             "--target", "install"], environment)

    log(f'Build completed: preset "{preset}".')
    return 0


if __name__ == "__main__":
    os.chdir(ROOT)
    sys.exit(main(sys.argv[1:]))
