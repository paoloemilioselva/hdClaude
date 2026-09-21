# ---------------------------------------------------------------------------
# hdClaudeCore
#
# No Vulkan, no OpenUSD, no third-party dependency. This is deliberate: the
# colour pipeline, the spectral sensor, hashing, and the shader cache are the
# parts most worth testing exhaustively, and they are testable on any host with
# a compiler. The core-only preset exists to run those tests.
# ---------------------------------------------------------------------------

add_library(hdClaudeCore STATIC
    curve_sweep.cpp
    display.cpp
    gaussian_splats.cpp
    hash.cpp
    image_metrics.cpp
    quad_tessellation.cpp
    shader_cache.cpp
    sphere_mesh.cpp
    spectrum.cpp
    tessellation.cpp
)

add_library(hdClaude::Core ALIAS hdClaudeCore)

target_include_directories(hdClaudeCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(hdClaudeCore PUBLIC hdClaudeProjectOptions)

set_target_properties(hdClaudeCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
