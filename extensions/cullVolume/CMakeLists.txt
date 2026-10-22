# ---------------------------------------------------------------------------
# Cull volumes: the usdCullVolume schema and its UsdImaging adapter.
#
# Kept apart from hdClaude because it is meant to be proposed to OpenUSD, and
# hdClaude does not link it: the render delegate reads the published data by
# name, as it does every other schema's (docs/cull-volumes.md).
#
# Both are assembled into the same plugin tree as hdClaude, which the
# plugin/usd/plugInfo.json written by src/hydra already covers with its
# "*/resources/" include:
#
#   plugin/usd/usdCullVolume/resources/plugInfo.json        (codeless schema)
#   plugin/usd/usdCullVolume/resources/generatedSchema.usda
#   plugin/usd/usdImagingCullVolume.dll                      (.so on Linux)
#   plugin/usd/usdImagingCullVolume/resources/plugInfo.json
# ---------------------------------------------------------------------------

add_library(usdImagingCullVolume MODULE
    usdImagingCullVolume/cullVolumeAdapter.cpp
    usdImagingCullVolume/cullVolumeSchema.cpp
)

target_include_directories(usdImagingCullVolume PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(usdImagingCullVolume
    PRIVATE
        hdClaudeProjectOptions
        hd
        sdf
        tf
        usd
        usdImaging
        vt
)

target_compile_definitions(usdImagingCullVolume PRIVATE USDIMAGINGCULLVOLUME_EXPORTS)

# The same allowance src/hydra makes for OpenUSD's own headers.
if(MSVC)
  target_compile_options(usdImagingCullVolume PRIVATE /wd4244 /wd4267 /wd4275 /wd4251)
endif()

set_target_properties(usdImagingCullVolume PROPERTIES
    PREFIX ""
    OUTPUT_NAME "usdImagingCullVolume"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin/usd"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin/usd"
)
foreach(_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_config}" _configUpper)
  set_target_properties(usdImagingCullVolume PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_configUpper} "${CMAKE_BINARY_DIR}/plugin/usd"
      RUNTIME_OUTPUT_DIRECTORY_${_configUpper} "${CMAKE_BINARY_DIR}/plugin/usd"
  )
endforeach()

set(_adapterResources "${CMAKE_BINARY_DIR}/plugin/usd/usdImagingCullVolume/resources")
set(_schemaResources "${CMAKE_BINARY_DIR}/plugin/usd/usdCullVolume/resources")

file(GENERATE
    OUTPUT "${_adapterResources}/plugInfo.json"
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/usdImagingCullVolume/plugInfo.json.in")

add_custom_command(TARGET usdImagingCullVolume POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_schemaResources}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/usdCullVolume/plugInfo.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/usdCullVolume/generatedSchema.usda"
            "${_schemaResources}"
    COMMENT "usdCullVolume: assembling the schema plugin in ${_schemaResources}"
    VERBATIM
)

install(TARGETS usdImagingCullVolume
    LIBRARY DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}"
)
install(FILES "${_adapterResources}/plugInfo.json"
    DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}/usdImagingCullVolume/resources")
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/usdCullVolume/plugInfo.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/usdCullVolume/generatedSchema.usda"
    DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}/usdCullVolume/resources")
