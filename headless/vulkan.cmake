if(NOT PS5_NATIVE)
    message(FATAL_ERROR "EDEN_PS5_VULKAN requires PS5_NATIVE")
endif()
# Eden's Lossless Scaling frame generation. Upstream gates it behind ENABLE_LSFG,
# which cmake_dependent_option pins to the "ANDROID" condition (CMakeLists.txt:
# "Only Android"), so -DENABLE_LSFG=ON alone is silently forced back OFF. Enable
# it here rather than with -DANDROID=ON: that would also flip ENABLE_OPENGL and
# other options OFF, and the console build needs the OpenGL renderer too.
# On in every build: without the user's own Lossless.dll it does nothing, and the launcher shows
# its settings greyed out (Services::frame_gen_state).
option(EDEN_PS5_FRAMEGEN "Build Eden's Lossless Scaling frame generation (LSFG)" ON)
# The HUD's shaders, as SPIR-V arrays in ${PORT_BUILD_DIR}/vulkan_hud_shaders.h (the window pass
# includes it). The rest of the Vulkan port is part of Eglantine/ itself.
execute_process(COMMAND python3 "${PORT_ROOT}/tools/build-hud-shaders.py" "${PORT_BUILD_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PORT_ROOT}/tools/build-hud-shaders.py"
    "${EDEN_PORT_DIR}/vulkan_hud.vert" "${EDEN_PORT_DIR}/vulkan_hud.frag")
target_include_directories(video_core PRIVATE "${PORT_BUILD_DIR}" "${EDEN_PORT_DIR}")
# The console's display plane instead of the desktop window systems (headless/vulkan_surface.cpp
# replaces vulkan_common/vulkan_surface.cpp), and the driver's host allocation callbacks.
list(REMOVE_ITEM video_sources vulkan_common/vulkan_surface.cpp)
list(APPEND video_sources "${EDEN_PORT_DIR}/vulkan_surface.cpp" "${EDEN_PORT_DIR}/vulkan_allocator.cpp")
target_compile_definitions(video_core PRIVATE PS5_NATIVE=1)
# Keep incompatible Mesa/PSBC globals private to the Vulkan archives.
if(EDEN_VULKAN_DRIVER STREQUAL "RADV")
    set(radv_archive "${PORT_ROOT}/build/radv-isolated/libvulkan_radeon.ps5.a")
    if(NOT EXISTS "${radv_archive}")
        message(FATAL_ERROR "Build and isolate the pinned RADV release archive first")
    endif()
    target_link_libraries(video_core PRIVATE "${radv_archive}"
        "${PORT_ROOT}/build/stubs/libSceAgcDriver.so"
        "${sdk}/target/lib/libSceSysmodule.so")
else()
set(vk_isolated "${PORT_ROOT}/build/vulkan-isolated")
foreach(archive libps5vk.a libpsbc.a)
    if(NOT EXISTS "${vk_isolated}/${archive}")
        message(FATAL_ERROR "Stage isolated Vulkan compiler archives before building")
    endif()
endforeach()
target_link_libraries(video_core PRIVATE "${vk_isolated}/libps5vk.a" "${vk_isolated}/libpsbc.a"
    "${PORT_ROOT}/build/stubs/libSceAgcDriver.so"
    "${sdk}/target/lib/libSceSysmodule.so")
endif()

# Lossless Scaling frame generation (EDEN_PS5_FRAMEGEN above). These are the
# sources upstream adds under "if (ENABLE_LSFG)" in src/video_core/CMakeLists.txt;
# paths are relative to src/video_core, like everything else in video_sources.
if(EDEN_PS5_FRAMEGEN)
    list(APPEND video_sources
        frame_gen/lossless_dll.cpp
        frame_gen/lsfg_translate.cpp
        renderer_vulkan/present/fsr3_frame_gen.cpp
        renderer_vulkan/present/frame_gen_pacer.cpp
        renderer_vulkan/present/lsfg_alpha.cpp
        renderer_vulkan/present/lsfg_beta.cpp
        renderer_vulkan/present/lsfg_chain.cpp
        renderer_vulkan/present/lsfg_common.cpp
        renderer_vulkan/present/lsfg_delta.cpp
        renderer_vulkan/present/lsfg_gamma.cpp
        renderer_vulkan/present/lsfg_generate.cpp
        renderer_vulkan/present/lsfg_mipmaps.cpp)
    # Eden's own gate for those sources. PUBLIC, exactly as upstream declares it
    # (src/video_core/CMakeLists.txt): the flag changes members of RendererVulkan
    # and PresentManager, so every consumer of their headers must see the same
    # layout.
    target_compile_definitions(video_core PUBLIC HAS_LSFG)
    # The port's frontend gate, so headless/main.cpp can ask for frame generation
    # (and say so in the log). eden-headless is created after this file is
    # included, so a directory-level definition reaches it.
    add_compile_definitions(EDEN_PS5_FRAMEGEN=1)
endif()
