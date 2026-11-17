# ---------------------------------------------------------------------------
# Tests
#
# Core tests require neither a GPU nor an OpenUSD runtime and always build.
# GPU and Hydra tests are added alongside their subsystems as those land.
# ---------------------------------------------------------------------------

add_executable(hdClaudeCoreTests core_tests.cpp)
target_link_libraries(hdClaudeCoreTests PRIVATE hdClaude::Core)
target_include_directories(hdClaudeCoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME core COMMAND hdClaudeCoreTests)

if(HDCLAUDE_ENABLE_VULKAN)
  # Compilation needs glslang but neither a GPU nor OpenUSD, so a material's
  # generated code can be checked for compilability on any machine.
  add_executable(hdClaudeGlslCompilerTests glsl_compiler_tests.cpp)
  target_link_libraries(hdClaudeGlslCompilerTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGlslCompilerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # The kernels are compiled from the source tree, not the install, so a kernel
  # that does not compile fails here rather than at the first render.
  target_compile_definitions(hdClaudeGlslCompilerTests PRIVATE
      HDCLAUDE_SHADER_DIRECTORY="${CMAKE_SOURCE_DIR}/shaders")
  add_test(NAME glsl_compiler COMMAND hdClaudeGlslCompilerTests)

  # Geometry and traversal. Scenes are built programmatically, never read from
  # USD: the GPU backend has no USD dependency and geometry reaches it only
  # through the Scene struct the Hydra mesh adapter fills.
  add_executable(hdClaudeGeometryTests geometry_tests.cpp)
  target_link_libraries(hdClaudeGeometryTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGeometryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(hdClaudeGeometryTests PRIVATE
      HDCLAUDE_TRAVERSAL_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/traversal.comp.glsl")
  add_test(NAME geometry COMMAND hdClaudeGeometryTests)

  # The denoiser against images whose answer is known. No GPU of hdClaude's
  # and no OpenUSD; it loads the OIDN runtime the configure fetched, from the
  # package itself rather than the plugin tree, so it is what the pin names.
  if(HDCLAUDE_ENABLE_OIDN AND HDCLAUDE_OIDN_AVAILABLE)
    add_executable(hdClaudeDenoiseTests denoise_tests.cpp)
    target_link_libraries(hdClaudeDenoiseTests PRIVATE hdClaude::Denoise)
    target_include_directories(hdClaudeDenoiseTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
      set(_oidn_runtime_dir "${oidn_SOURCE_DIR}/bin")
    else()
      set(_oidn_runtime_dir "${oidn_SOURCE_DIR}/lib")
    endif()
    target_compile_definitions(hdClaudeDenoiseTests PRIVATE
        HDCLAUDE_OIDN_RUNTIME_DIRECTORY="${_oidn_runtime_dir}")
    add_test(NAME denoise COMMAND hdClaudeDenoiseTests)
  endif()

  add_executable(hdClaudeGpuTests gpu_tests.cpp)
  target_link_libraries(hdClaudeGpuTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu COMMAND hdClaudeGpuTests)

  # Validation is a gate, not a console message (docs/lessons-from-hdcodex.md
  # R8). The test binary enables the layer and fails both on a nonzero error
  # count and when the layer is unavailable, so the environment is pointed at
  # whichever layer the configure located or built.
  set(_gpu_test_env "HDCLAUDE_ENABLE_VULKAN_VALIDATION=1")
  if(HDCLAUDE_VALIDATION_LAYER_DIR)
    list(APPEND _gpu_test_env "VK_LAYER_PATH=${HDCLAUDE_VALIDATION_LAYER_DIR}")
  endif()
  # Off Windows the manifest names a bare library the loader finds on the
  # library search path, so the layer's lib directory travels with the tests.
  set(_layer_path_modification "")
  if(HDCLAUDE_VALIDATION_LAYER_LIBRARY_DIR)
    set(_layer_path_modification
        "LD_LIBRARY_PATH=path_list_prepend:${HDCLAUDE_VALIDATION_LAYER_LIBRARY_DIR}")
  endif()
  set_tests_properties(gpu geometry PROPERTIES
      SKIP_RETURN_CODE 77
      ENVIRONMENT "${_gpu_test_env}"
      ENVIRONMENT_MODIFICATION "${_layer_path_modification}")

  # The layer must exist before the test runs, so the test binary depends on
  # the external build rather than relying on build order by accident.
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeGpuTests hdClaudeValidationLayers)
    add_dependencies(hdClaudeGeometryTests hdClaudeValidationLayers)
  endif()
endif()

if(HDCLAUDE_ENABLE_MATERIALX)
  # MaterialX-linked tests load OpenUSD's DLLs at runtime. ctest does not
  # inherit whatever PATH a developer happened to set, so the tests carry it
  # themselves -- otherwise `ctest` fails with 0xC0000135 (DLL not found) while
  # running the same binary by hand succeeds, which is a confusing way to
  # discover a missing search path.
  # Runtime search path for OpenUSD's DLLs, and the validation environment.
  # Both are defined before any test uses them: a CMake variable read before it
  # is set expands to nothing and the test silently runs unconfigured.
  if(WIN32)
    set(_usd_path_modification
        "PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/bin"
        "PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/lib")
  else()
    set(_usd_path_modification
        "LD_LIBRARY_PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/lib")
  endif()
  list(APPEND _usd_path_modification ${_layer_path_modification})

  set(_validation_env "HDCLAUDE_ENABLE_VULKAN_VALIDATION=1")
  if(HDCLAUDE_VALIDATION_LAYER_DIR)
    list(APPEND _validation_env "VK_LAYER_PATH=${HDCLAUDE_VALIDATION_LAYER_DIR}")
  endif()
  # Generates a material through genglsl_pt and compiles it to SPIR-V. Needs the
  # MaterialX libraries from the OpenUSD distribution, but no GPU and no stage.
  add_executable(hdClaudeMaterialXTests materialx_tests.cpp)
  target_link_libraries(hdClaudeMaterialXTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeMaterialXTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME materialx COMMAND hdClaudeMaterialXTests)
  set_tests_properties(materialx PROPERTIES
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")

  # The phase 4 acceptance gate: generate each closure, compile it with a
  # validation kernel, and run it on the GPU. Asks whether the shading is
  # *right*, where every other test asks whether it compiles.
  add_executable(hdClaudeClosureValidationTests closure_validation_tests.cpp)
  target_link_libraries(hdClaudeClosureValidationTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeClosureValidationTests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(hdClaudeClosureValidationTests PRIVATE
      HDCLAUDE_VALIDATION_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/closure_validation.comp.glsl"
      HDCLAUDE_CHI2_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/closure_chi2.comp.glsl")
  add_test(NAME closure_validation COMMAND hdClaudeClosureValidationTests)
  set_tests_properties(closure_validation PROPERTIES SKIP_RETURN_CODE 77)

  # The first images: a MaterialX material shading real geometry through the
  # wavefront integrator. Asserts light transport -- lit brighter than unlit, a
  # shadow darker than its surroundings, a red surface red -- rather than pixel
  # values, because those hold for any correct renderer and none of them holds
  # for one that is merely producing output.
  add_executable(hdClaudeRenderTests render_tests.cpp)
  target_link_libraries(hdClaudeRenderTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeRenderTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME render COMMAND hdClaudeRenderTests)
  set_tests_properties(render PROPERTIES
      SKIP_RETURN_CODE 77
      ENVIRONMENT "${_validation_env}"
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeRenderTests hdClaudeValidationLayers)
  endif()

  # The Hydra adapter classes that stand alone -- a render buffer needs only an
  # id, an instancer only a scene delegate -- compiled from the plugin's own
  # sources, since nothing links against the plugin module. Tests the source
  # tree rather than the install.
  if(HDCLAUDE_BUILD_HYDRA_PLUGIN)
    add_executable(hdClaudeHydraTests
        hydra_tests.cpp
        ${CMAKE_SOURCE_DIR}/src/hydra/dds_image.cpp
        ${CMAKE_SOURCE_DIR}/src/hydra/instancer.cpp
        ${CMAKE_SOURCE_DIR}/src/hydra/render_buffer.cpp)
    target_include_directories(hdClaudeHydraTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/hydra
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(hdClaudeHydraTests PRIVATE
        hdClaudeProjectOptions hdClaude::Core hdClaudeBcdec gf hd sdf tf vt)
    if(MSVC)
      # The plugin's own suppressions, for the same OpenUSD headers.
      target_compile_options(hdClaudeHydraTests PRIVATE
          /wd4244 /wd4267 /wd4275 /wd4251)
    endif()
    add_test(NAME hydra COMMAND hdClaudeHydraTests)
    set_tests_properties(hydra PROPERTIES
        ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  endif()

  # Phase 3 and 8: instanced geometry renders where USD places it. The
  # scripted tests that reach the Hydra adapters, so each runs a script in the
  # OpenUSD environment, through the platform's wrapper, which sources
  # setup_usd_env first.
  #
  # Against the plugin this build assembled in plugin/usd, not the installed
  # one: HDCLAUDE_PLUGIN_DIR takes the install's place on the plugin path. An
  # install that failed -- a viewer holding the DLL is enough -- left these
  # testing whatever was installed before, and passing for it. Not for a
  # Houdini build, whose plugin is for Houdini's USD and not this one.
  set(_usd_test_env "HDCLAUDE_IMAGE_DIFF=$<TARGET_FILE:hdClaudeImageDiff>")
  if(NOT HDCLAUDE_HOUDINI_VERSION)
    list(APPEND _usd_test_env
         "HDCLAUDE_PLUGIN_DIR=${CMAKE_BINARY_DIR}/plugin/usd")
  endif()
  if(TARGET hdClaudeImageDiff)
    foreach(_usd_test
            "usd_instancing|instancing_equivalence"
            "usd_lights|light_linking_equivalence"
            "usd_cull_volumes|cull_volume_equivalence"
            "usd_ray_visibility|ray_visibility_equivalence"
            "usd_fog_volumes|fog_volumes"
            "usd_implicit_surfaces|implicit_surfaces"
            "usd_texture_quality|texture_quality"
            "usd_denoising|denoising"
            "usd_picking|picking"
            "usd_incremental_sync|incremental_sync"
            "usd_render_stats|render_stats"
            "usd_material_edits|material_edits"
            "usd_purposes|purposes"
            "usd_unusable_materials|unusable_materials"
            "usd_ribbon_curves|ribbon_curves"
            "usd_depth|depth"
            "usd_houdini_parameters|houdini_parameters"
            "usd_landing_page|landing_page"
            "usd_cornell_box|cornell_box_reference"
            "usd_physical_illuminant|physical_illuminant"
            "usd_physical_brightness|physical_brightness")
      string(REPLACE "|" ";" _usd_test "${_usd_test}")
      list(GET _usd_test 0 _name)
      list(GET _usd_test 1 _script)
      if(WIN32)
        set(_command "${CMAKE_CURRENT_SOURCE_DIR}/usd/run_${_script}.bat")
      else()
        set(_command bash "${CMAKE_CURRENT_SOURCE_DIR}/usd/run_in_usd_env.sh"
                          "${_script}.py")
      endif()
      add_test(NAME ${_name}
          COMMAND ${_command} "${CMAKE_CURRENT_BINARY_DIR}/${_name}")
      set_tests_properties(${_name} PROPERTIES
          ENVIRONMENT "${_usd_test_env}")
    endforeach()
  endif()

  set_tests_properties(closure_validation PROPERTIES
      ENVIRONMENT "${_validation_env}"
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeClosureValidationTests hdClaudeValidationLayers)
  endif()
endif()

# Every test gets a cache of its own build tree. Left to the default, a test
# would read the user's cache, which a host session filled -- so a kernel or
# material that no longer compiles could pass here on an entry compiled before
# it broke -- and would write test-only entries into it. Appended last, after
# every set_tests_properties above, because ENVIRONMENT set there replaces.
get_property(_hdclaude_tests DIRECTORY PROPERTY TESTS)
if(_hdclaude_tests)
  set_property(TEST ${_hdclaude_tests} APPEND PROPERTY ENVIRONMENT
      "HDCLAUDE_SHADER_CACHE=${CMAKE_CURRENT_BINARY_DIR}/shader-cache")
endif()
