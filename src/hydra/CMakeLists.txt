# ---------------------------------------------------------------------------
# hdClaude
#
# The Hydra render delegate. This is the only library in the project that
# includes both `pxr` headers and hdClaude's GPU layer, which is the point: it
# is the seam between OpenUSD and the renderer, and everything below it is
# testable without a USD runtime (docs/architecture.md 3).
#
# Built as a MODULE, not a SHARED library. Nothing links against a Hydra
# plugin; it is discovered and dlopened by the Plug system, and declaring it a
# module keeps CMake from generating an import library nothing would use.
# ---------------------------------------------------------------------------

add_library(hdClaude MODULE
    camera.cpp
    light.cpp
    material.cpp
    material_compiler.cpp
    mesh.cpp
    render_buffer.cpp
    render_delegate.cpp
    render_pass.cpp
    renderer_plugin.cpp
    scene_store.cpp
    texture_loader.cpp
)

target_include_directories(hdClaude PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hdClaude
    PRIVATE
        hdClaudeProjectOptions
        hdClaude::Core
        hdClaude::Gpu
        hdClaude::MaterialX
        ar
        hd
        hdMtlx
        hf
        hio
        plug
        pxOsd
        sdf
        tf
        trace
        usd
        usdLux
        vt
)

target_compile_definitions(hdClaude PRIVATE HDCLAUDE_EXPORTS)

# OpenUSD's headers are not warning-clean at /W4, and hdClaude's own warnings
# are the ones worth reading. This narrows the noise without lowering the bar
# for our own code, which is compiled with the same options everywhere else.
if(MSVC)
  target_compile_options(hdClaude PRIVATE /wd4244 /wd4267 /wd4275 /wd4251)
endif()

# The plugin's own name, with no lib prefix: Plug matches the LibraryPath in
# plugInfo.json literally, so a "libhdClaude.so" would simply not be found.
set_target_properties(hdClaude PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hdClaude"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin/usd"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin/usd"
)
foreach(_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_config}" _configUpper)
  set_target_properties(hdClaude PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_configUpper} "${CMAKE_BINARY_DIR}/plugin/usd"
      RUNTIME_OUTPUT_DIRECTORY_${_configUpper} "${CMAKE_BINARY_DIR}/plugin/usd"
  )
endforeach()

# ---------------------------------------------------------------------------
# Plugin layout.
#
# Plug scans each PXR_PLUGINPATH_NAME entry for */resources/plugInfo.json, and
# resolves LibraryPath relative to Root -- which plugInfo.json sets to "..",
# the directory *above* the plugin's own. So the library sits beside the
# resource directory, not inside it, exactly as hdEmbree and hdStorm do:
#
#   plugin/usd/hdClaude.dll
#   plugin/usd/hdClaude/resources/plugInfo.json
#   plugin/usd/hdClaude/resources/shaders/*.glsl
#   plugin/usd/hdClaude/resources/mtlx/...
#
# Assembled at build time as well as at install, so the plugin is usable
# straight from the build tree -- which is how it is actually tested.
# ---------------------------------------------------------------------------
set(HDCLAUDE_PLUGIN_RESOURCES "${CMAKE_BINARY_DIR}/plugin/usd/hdClaude/resources")

add_custom_command(TARGET hdClaude POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/plugInfo.json"
            "${HDCLAUDE_PLUGIN_RESOURCES}/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/shaders"
            "${HDCLAUDE_PLUGIN_RESOURCES}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/mtlx"
            "${HDCLAUDE_PLUGIN_RESOURCES}/mtlx"
    COMMENT "hdClaude: assembling the plugin tree in ${CMAKE_BINARY_DIR}/plugin/usd"
    VERBATIM
)

install(TARGETS hdClaude
    LIBRARY DESTINATION "plugin/usd"
    RUNTIME DESTINATION "plugin/usd"
)
install(FILES "${CMAKE_SOURCE_DIR}/resources/plugInfo.json"
    DESTINATION "plugin/usd/hdClaude/resources")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/"
    DESTINATION "plugin/usd/hdClaude/resources/shaders")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/mtlx/"
    DESTINATION "plugin/usd/hdClaude/resources/mtlx")
