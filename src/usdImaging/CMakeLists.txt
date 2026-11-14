# ---------------------------------------------------------------------------
# usdImagingHdClaude: the UsdImaging adapter that publishes the stage's metres
# per unit and up axis on every light (stageMetricsAdapter.cpp).
#
# A module of its own, assembled into the same plugin tree as hdClaude and
# found through the same plugin/usd/plugInfo.json "*/resources/" include:
#
#   plugin/usd/usdImagingHdClaude.dll                    (.so on Linux)
#   plugin/usd/usdImagingHdClaude/resources/plugInfo.json
# ---------------------------------------------------------------------------

add_library(usdImagingHdClaude MODULE
    stageMetricsAdapter.cpp
)

target_link_libraries(usdImagingHdClaude
    PRIVATE
        hdClaudeProjectOptions
        hd
        sdf
        tf
        usd
        usdGeom
        usdLux
        usdImaging
        vt
)

# The same allowance src/hydra makes for OpenUSD's own headers.
if(MSVC)
  target_compile_options(usdImagingHdClaude PRIVATE /wd4244 /wd4267 /wd4275 /wd4251)
endif()

set_target_properties(usdImagingHdClaude PROPERTIES
    PREFIX ""
    OUTPUT_NAME "usdImagingHdClaude"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin/usd"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin/usd"
)
foreach(_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_config}" _configUpper)
  set_target_properties(usdImagingHdClaude PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_configUpper} "${CMAKE_BINARY_DIR}/plugin/usd"
      RUNTIME_OUTPUT_DIRECTORY_${_configUpper} "${CMAKE_BINARY_DIR}/plugin/usd"
  )
endforeach()

set(_resources "${CMAKE_BINARY_DIR}/plugin/usd/usdImagingHdClaude/resources")
file(GENERATE
    OUTPUT "${_resources}/plugInfo.json"
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in")

install(TARGETS usdImagingHdClaude
    LIBRARY DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}"
)
install(FILES "${_resources}/plugInfo.json"
    DESTINATION "${HDCLAUDE_PLUGIN_INSTALL_DIR}/usdImagingHdClaude/resources")
