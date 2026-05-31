cmake_minimum_required(VERSION 3.20)

project(Game)

if (NOT IS_DIRECTORY $ENV{VULKAN_SDK})
    message(WARNING "Vulkan SDK NOT  DETECTED")
else()
    message(STATUS "Found Vulkan SDK in $ENV{VULKAN_SDK}")
endif()

# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# This assumes the SDL source is available in extern/SDL
add_subdirectory(extern/SDL EXCLUDE_FROM_ALL)
add_subdirectory(extern/KTX-Software/lib ktx)

if (UNIX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -DVK_USE_PLATFORM_WAYLAND_KHR")
    set(SLANGC "$ENV{VULKAN_SDK}/bin/slangc")
endif()

file(GLOB_RECURSE SLANG_SOURCE_FILES "../assets/*slang")

set(SPIRV_OUTPUT_DIR "${PROJECT_BINARY_DIR}/../spirv/")
foreach(SLANG ${SLANG_SOURCE_FILES})
    get_filename_component(STEM ${SLANG} NAME_WLE)
    set(SPIRV "${SPIRV_OUTPUT_DIR}${STEM}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        DEPENDS ${SLANG} "../assets/mesh.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SPIRV_OUTPUT_DIR}"
        COMMAND ${SLANGC} ${SLANG} -target spirv -fvk-use-scalar-layout -o ${SPIRV})
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach()

add_library(Game SHARED "game.c"
                        "log.c"
                        "og_ds.c"
                        "platform.c"
                        "renderer.c"
                        "string_intern.c"
                        "hexgrid.h"
                        "hexgrid.c"
                        "vulkan/vulkan.c"
                        "vulkan/buffer.c"
                        "vulkan/shader.c"
                        "vulkan/texture.c"
                        "vulkan/vma.cpp"
                        "vulkan/volk.c")

target_link_libraries(Game PRIVATE SDL3::SDL3 ktx)
target_include_directories(Game PRIVATE "extern")

add_custom_target(compile_shaders DEPENDS ${SPIRV_BINARY_FILES})
add_dependencies(Game compile_shaders)