# ---------------------------------------------------------------------------
# hdClaudeMaterialX
#
# The C++ half of the genglsl_pt target: syntax, generator, and the surface
# node's calling convention. The other half is the data library under mtlx/.
#
# Links MaterialX from inside the OpenUSD distribution rather than a separate
# copy -- two MaterialX libraries in one process is an ODR violation waiting to
# happen, and Hydra hands us objects created by its own copy.
# ---------------------------------------------------------------------------

add_library(hdClaudeMaterialX STATIC
    pathtracer_generator.cpp
)

add_library(hdClaude::MaterialX ALIAS hdClaudeMaterialX)

target_include_directories(hdClaudeMaterialX PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(hdClaudeMaterialX
    PUBLIC
        hdClaudeProjectOptions
        hdClaude::Core
        hdClaude::Gpu
        MaterialXCore
        MaterialXFormat
        MaterialXGenShader
        MaterialXGenGlsl
)

# The data library location is compiled in so a test or tool can find it
# without an environment variable, while MATERIALX_SEARCH_PATH still overrides.
target_compile_definitions(hdClaudeMaterialX PUBLIC
    HDCLAUDE_MTLX_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/mtlx"
    HDCLAUDE_MATERIALX_STDLIB_DIR="${HDCLAUDE_MATERIALX_LIBRARIES}"
)

set_target_properties(hdClaudeMaterialX PROPERTIES POSITION_INDEPENDENT_CODE ON)
