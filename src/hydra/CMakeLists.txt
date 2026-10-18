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
    instancer.cpp
    light.cpp
    implicit_surface_scene_index_plugin.cpp
    light_linking_scene_index_plugin.cpp
    material.cpp
    material_compiler.cpp
    basis_curves.cpp
    mesh.cpp
    nurbs_scene_index_plugin.cpp
    particle_field.cpp
    retessellation_scene_index_plugin.cpp
    sphere_scene_index.cpp
    render_buffer.cpp
    render_delegate.cpp
    render_pass.cpp
    renderer_plugin.cpp
    scene_store.cpp
    subdivision.cpp
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
        hdsi
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
# Plug reads plugInfo.json from each PXR_PLUGINPATH_NAME entry and does not
# scan below it, so plugin/usd carries one whose Includes reaches the plugin's
# own, exactly as OpenUSD's plugin/usd does. Plug resolves LibraryPath relative
# to Root -- which the plugin's plugInfo.json sets to "..",
# the directory *above* the plugin's own. So the library sits beside the
# resource directory, not inside it, exactly as hdEmbree and hdStorm do:
#
#   plugin/usd/plugInfo.json  {"Includes": ["*/resources/"]}
#   plugin/usd/hdClaude.dll  (hdClaude.so on Linux)
#   plugin/usd/hdClaude/resources/plugInfo.json
#   plugin/usd/hdClaude/resources/shaders/*.glsl
#   plugin/usd/hdClaude/resources/mtlx/...
#
# Assembled at build time as well as at install, so the plugin is usable
# straight from the build tree -- which is how it is actually tested.
# ---------------------------------------------------------------------------
set(HDCLAUDE_PLUGIN_RESOURCES "${CMAKE_BINARY_DIR}/plugin/usd/hdClaude/resources")

# LibraryPath must name the module exactly as the linker wrote it -- hdClaude.dll
# on Windows, hdClaude.so on Linux -- so it is generated from the target rather
# than written by hand. A hard-coded name loads on one platform and is silently
# not found on the other.
file(GENERATE
    OUTPUT "${HDCLAUDE_PLUGIN_RESOURCES}/plugInfo.json"
    INPUT  "${CMAKE_SOURCE_DIR}/resources/plugInfo.json.in")
file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/plugin/usd/plugInfo.json"
    CONTENT "{\n    \"Includes\": [ \"*/resources/\" ]\n}\n")

add_custom_command(TARGET hdClaude POST_BUILD
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
install(FILES "${HDCLAUDE_PLUGIN_RESOURCES}/plugInfo.json"
    DESTINATION "plugin/usd/hdClaude/resources")
install(FILES "${CMAKE_BINARY_DIR}/plugin/usd/plugInfo.json"
    DESTINATION "plugin/usd")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/"
    DESTINATION "plugin/usd/hdClaude/resources/shaders")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/mtlx/"
    DESTINATION "plugin/usd/hdClaude/resources/mtlx")
