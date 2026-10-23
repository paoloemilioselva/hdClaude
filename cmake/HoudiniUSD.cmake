# ---------------------------------------------------------------------------
# OpenUSD and MaterialX from a Houdini installation.
#
# The Houdini-ABI build that cmake/OpenUSD.cmake keeps out of itself: a
# delegate Solaris and husk can load has to link the USD and MaterialX that
# Houdini ships -- its own namespace (pxrInternal_v0_26_5__...), its own TBB,
# its own Python -- and not a standalone build of the same version.
#
# Houdini installs no pxrConfig.cmake, and its HoudiniConfig.cmake describes
# the HDK: hundreds of libraries, /W1 and the HDK's defines, none of which a
# Hydra plugin uses. So this defines, over Houdini's import libraries, the
# same target names pxrConfig.cmake and MaterialXConfig.cmake would (`hd`,
# `usd`, `MaterialXCore`, ...), and nothing else in the build has to know
# which USD it is building against.
#
# Selected by HDCLAUDE_HOUDINI_VERSION (the houdini-22.0 preset). The root is
# HDCLAUDE_HOUDINI_ROOT, else $HFS when it is that version, else the newest
# installed build of that version.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)

set(HDCLAUDE_HOUDINI_ROOT "" CACHE PATH
    "Houdini installation ($HFS). Empty: found from HDCLAUDE_HOUDINI_VERSION.")

if(NOT HDCLAUDE_HOUDINI_ROOT)
  set(_candidates "")
  if(DEFINED ENV{HFS})
    file(TO_CMAKE_PATH "$ENV{HFS}" _hfs)
    list(APPEND _candidates "${_hfs}")
  endif()
  # Newest first: a natural sort orders 22.0.469 after 22.0.98.
  if(WIN32)
    file(GLOB _installed LIST_DIRECTORIES true
         "C:/Program Files/Side Effects Software/Houdini ${HDCLAUDE_HOUDINI_VERSION}.*")
  elseif(APPLE)
    file(GLOB _installed LIST_DIRECTORIES true
         "/Applications/Houdini/Houdini${HDCLAUDE_HOUDINI_VERSION}.*/Frameworks/Houdini.framework/Versions/Current/Resources")
  else()
    file(GLOB _installed LIST_DIRECTORIES true
         "/opt/hfs${HDCLAUDE_HOUDINI_VERSION}.*")
  endif()
  list(SORT _installed COMPARE NATURAL ORDER DESCENDING)
  list(APPEND _candidates ${_installed})
  foreach(_candidate ${_candidates})
    if(NOT HDCLAUDE_HOUDINI_ROOT AND
       EXISTS "${_candidate}/toolkit/cmake/HoudiniConfigVersion.cmake")
      file(STRINGS "${_candidate}/toolkit/cmake/HoudiniConfigVersion.cmake"
           _version REGEX "set\\( *PACKAGE_VERSION ")
      string(REGEX MATCH "[0-9]+\\.[0-9]+" _version "${_version}")
      if(_version STREQUAL HDCLAUDE_HOUDINI_VERSION)
        set(HDCLAUDE_HOUDINI_ROOT "${_candidate}" CACHE PATH "" FORCE)
      endif()
    endif()
  endforeach()
endif()

if(NOT EXISTS "${HDCLAUDE_HOUDINI_ROOT}/toolkit/include/pxr/pxr.h")
  message(FATAL_ERROR
      "Houdini ${HDCLAUDE_HOUDINI_VERSION} was not found (HDCLAUDE_HOUDINI_ROOT "
      "is '${HDCLAUDE_HOUDINI_ROOT}', HFS is '$ENV{HFS}').\n"
      "Set -DHDCLAUDE_HOUDINI_ROOT=<Houdini install root> ($HFS). "
      "See docs/building.md.")
endif()

set(_hfs "${HDCLAUDE_HOUDINI_ROOT}")
if(WIN32)
  set(_libdir "${_hfs}/custom/houdini/dsolib")
  set(_bindir "${_hfs}/bin")
else()
  set(_libdir "${_hfs}/dsolib")
  set(_bindir "${_hfs}/dsolib")
endif()

# The Python Houdini's USD was built with. Its headers include Python.h, whose
# pyconfig.h asks the linker for the import library by name.
file(GLOB _python_includes LIST_DIRECTORIES true "${_hfs}/toolkit/include/python3.*")
list(GET _python_includes 0 _python_include)
cmake_path(GET _python_include FILENAME _python)              # python3.13
string(REPLACE "python" "" HDCLAUDE_HOUDINI_PYTHON "${_python}")
string(REPLACE "." "" _python_dotless "${HDCLAUDE_HOUDINI_PYTHON}")

# --- What every USD and MaterialX target carries -----------------------------
add_library(hdClaudeHoudiniUsd INTERFACE)
target_include_directories(hdClaudeHoudiniUsd SYSTEM INTERFACE
    "${_hfs}/toolkit/include"
    "${_python_include}")
# TBB links itself by #pragma from its headers, so its directory is enough.
# Houdini's pyconfig.h does not do the same for Python, whose library is named.
target_link_directories(hdClaudeHoudiniUsd INTERFACE "${_libdir}")
find_library(HDCLAUDE_HOUDINI_python
    NAMES "python${_python_dotless}"
    PATHS "${_hfs}/python${_python_dotless}/libs" "${_libdir}"
    NO_DEFAULT_PATH REQUIRED)
target_link_libraries(hdClaudeHoudiniUsd INTERFACE "${HDCLAUDE_HOUDINI_python}")
target_compile_definitions(hdClaudeHoudiniUsd INTERFACE
    PXR_BOOST_PYTHON_NO_PY_SIGNATURES)

# Every USD library a target here names, or one it uses inline through
# another's headers. Linked together rather than as pxrTargets.cmake's graph:
# an import library nothing references costs nothing, and a missing one
# fails at link time with the symbol it is missing.
set(_pxr_libraries
    arch tf gf js trace work plug vt ar kind sdf pcp sdr ts
    usd usdGeom usdShade usdLux usdHydra usdMtlx usdImaging
    cameraUtil geomUtil hd hdsi hdMtlx hf hio pxOsd python)
foreach(_library ${_pxr_libraries})
  find_library(HDCLAUDE_HOUDINI_pxr_${_library}
      NAMES "libpxr_${_library}" "pxr_${_library}"
      PATHS "${_libdir}" NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(hdClaudeHoudiniUsd INTERFACE
      "${HDCLAUDE_HOUDINI_pxr_${_library}}")
endforeach()

# OpenSubdiv, which hdClaude's refinement calls directly. Houdini links it
# statically into its USD, and ships the same static library.
find_library(HDCLAUDE_HOUDINI_osdCPU
    NAMES libosdCPU_md osdCPU libosdCPU
    PATHS "${_libdir}" NO_DEFAULT_PATH REQUIRED)
target_link_libraries(hdClaudeHoudiniUsd INTERFACE "${HDCLAUDE_HOUDINI_osdCPU}")

# The names the rest of the build links, as pxrConfig.cmake spells them.
foreach(_library ${_pxr_libraries})
  if(NOT TARGET ${_library})
    add_library(${_library} INTERFACE IMPORTED GLOBAL)
    target_link_libraries(${_library} INTERFACE hdClaudeHoudiniUsd)
  endif()
endforeach()

# --- MaterialX -----------------------------------------------------------------
if(HDCLAUDE_ENABLE_MATERIALX)
  foreach(_library MaterialXCore MaterialXFormat MaterialXGenShader MaterialXGenGlsl)
    find_library(HDCLAUDE_HOUDINI_${_library}
        NAMES ${_library} PATHS "${_libdir}" NO_DEFAULT_PATH REQUIRED)
    add_library(${_library} INTERFACE IMPORTED GLOBAL)
    target_link_libraries(${_library} INTERFACE
        hdClaudeHoudiniUsd "${HDCLAUDE_HOUDINI_${_library}}")
  endforeach()
  # Split out of MaterialXGenShader in 1.39.5, so only some Houdinis have it.
  find_library(HDCLAUDE_HOUDINI_MaterialXGenHw
      NAMES MaterialXGenHw PATHS "${_libdir}" NO_DEFAULT_PATH)
  if(HDCLAUDE_HOUDINI_MaterialXGenHw)
    add_library(MaterialXGenHw INTERFACE IMPORTED GLOBAL)
    target_link_libraries(MaterialXGenHw INTERFACE
        hdClaudeHoudiniUsd "${HDCLAUDE_HOUDINI_MaterialXGenHw}")
  endif()

  file(STRINGS "${_hfs}/toolkit/include/MaterialXCore/Generated.h" _mx_version
       REGEX "#define MATERIALX_(MAJOR|MINOR|BUILD)_VERSION")
  string(REGEX MATCHALL "[0-9]+" _mx_version "${_mx_version}")
  list(JOIN _mx_version "." MaterialX_VERSION)
  # Validated against 1.39.3 (OpenUSD 26.03) and 1.39.5 (Houdini 22.0): the
  # shader balls render the same under both (implementation-notes.md,
  # 2026-10-23). Any other release is untested, and said so at every configure.
  if(NOT MaterialX_VERSION VERSION_EQUAL "1.39.3" AND
     NOT MaterialX_VERSION VERSION_EQUAL "1.39.5")
    message(WARNING
        "hdClaude's genglsl_pt target is validated against MaterialX 1.39.3 "
        "and 1.39.5; Houdini ${HDCLAUDE_HOUDINI_VERSION} has "
        "${MaterialX_VERSION}. Materials may compile and still not shade as "
        "they do under those: see docs/implementation-notes.md, 2026-10-23, "
        "\"MaterialX 1.39.5 rewrote the graph before hdClaude saw it\".")
  endif()

  # Compiled into the plugin, as with a standalone USD: the data libraries of
  # the MaterialX it runs against.
  unset(HDCLAUDE_MATERIALX_LIBRARIES CACHE)
  set(HDCLAUDE_MATERIALX_LIBRARIES "${_hfs}/houdini/materialx/libraries")
  if(NOT EXISTS "${HDCLAUDE_MATERIALX_LIBRARIES}/pbrlib/genglsl")
    message(FATAL_ERROR
        "MaterialX standard libraries not found at "
        "'${HDCLAUDE_MATERIALX_LIBRARIES}'.")
  endif()
endif()

find_package(OpenGL REQUIRED)

file(STRINGS "${_hfs}/toolkit/include/pxr/pxr.h" PXR_VERSION
     REGEX "#define PXR_VERSION ")
string(REGEX MATCH "[0-9]+" PXR_VERSION "${PXR_VERSION}")

# Where Houdini looks for it: a package in the user preferences adds the
# install directory to HOUDINI_PATH, and Houdini puts each HOUDINI_PATH
# entry's dso/usd_plugins on the USD plugin path.
#
# Houdini's own answer, in its order: $HOUDINI_USER_PREF_DIR, with __HVER__
# standing for the version, else houdiniX.Y in $HOME -- which on Windows is
# the Documents known folder, not %USERPROFILE%\Documents. The two differ
# whenever OneDrive has redirected Documents, and installing into the second
# puts the package where Houdini never looks.
set(HDCLAUDE_HOUDINI_USER_DIR "" CACHE PATH
    "Houdini user preference directory. Empty: the one Houdini itself uses.")
if(HDCLAUDE_HOUDINI_USER_DIR)
  set(_user_dir "${HDCLAUDE_HOUDINI_USER_DIR}")
elseif(DEFINED ENV{HOUDINI_USER_PREF_DIR})
  string(REPLACE "__HVER__" "${HDCLAUDE_HOUDINI_VERSION}" _user_dir
         "$ENV{HOUDINI_USER_PREF_DIR}")
else()
  if(WIN32)
    # "Shell Folders" holds the resolved path; "User Shell Folders" holds it
    # with %USERPROFILE% unexpanded.
    cmake_host_system_information(RESULT _documents
        QUERY WINDOWS_REGISTRY
        "HKCU/Software/Microsoft/Windows/CurrentVersion/Explorer/Shell Folders"
        VALUE "Personal")
    if(NOT _documents)
      set(_documents "$ENV{USERPROFILE}/Documents")
    endif()
  elseif(APPLE)
    set(_documents "$ENV{HOME}/Library/Preferences/houdini")
  else()
    set(_documents "$ENV{HOME}")
  endif()
  set(_user_dir "${_documents}/houdini${HDCLAUDE_HOUDINI_VERSION}")
endif()
file(TO_CMAKE_PATH "${_user_dir}" HDCLAUDE_HOUDINI_USER_PREF_DIR)
message(STATUS "hdClaude: Houdini user preferences at ${HDCLAUDE_HOUDINI_USER_PREF_DIR}")

message(STATUS "hdClaude: Houdini ${HDCLAUDE_HOUDINI_VERSION} at ${_hfs}")
message(STATUS "hdClaude: its OpenUSD ${PXR_VERSION}, MaterialX ${MaterialX_VERSION}, "
               "Python ${HDCLAUDE_HOUDINI_PYTHON}")
