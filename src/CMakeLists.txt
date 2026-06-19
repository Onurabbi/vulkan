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
set(MESHOPT_BUILD_SHARED_LIBS ON)
add_subdirectory(extern/meshoptimizer EXCLUDE_FROM_ALL)

if (UNIX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -DVK_USE_PLATFORM_WAYLAND_KHR")
    set(SLANGC "$ENV{VULKAN_SDK}/bin/slangc")
endif()

set(ASSET_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets")
set(ASSET_OUTPUT_DIR "${PROJECT_BINARY_DIR}/../assets")

file(GLOB_RECURSE SLANG_SOURCE_FILES "${ASSET_SOURCE_DIR}/shaders/*.slang")

set(SPIRV_BINARY_FILES "")
foreach(SLANG_FILE ${SLANG_SOURCE_FILES})
    string(REPLACE "${ASSET_SOURCE_DIR}/shaders/" "" RELATIVE_SLANG_FILE ${SLANG_FILE})
    string(REPLACE ".slang" ".spv" SPIRV_FILE_REL ${RELATIVE_SLANG_FILE})
    set(SPIRV_FILE_ABS "${ASSET_OUTPUT_DIR}/shaders/${SPIRV_FILE_REL}")
    get_filename_component(SPIRV_DIR ${SPIRV_FILE_ABS} DIRECTORY)

    add_custom_command(
        OUTPUT ${SPIRV_FILE_ABS}
        DEPENDS ${SLANG_FILE} "${ASSET_SOURCE_DIR}/shaders/mesh.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SPIRV_DIR}"
        COMMAND ${SLANGC} ${SLANG_FILE} -target spirv -fvk-use-scalar-layout -o ${SPIRV_FILE_ABS}
        COMMENT "Compiling ${SLANG_FILE} to ${SPIRV_FILE_ABS}")
    list(APPEND SPIRV_BINARY_FILES ${SPIRV_FILE_ABS})
endforeach()

file(GLOB_RECURSE ASSET_FILES "${ASSET_SOURCE_DIR}/meshes/*"
                              "${ASSET_SOURCE_DIR}/textures/*")

set(COPIED_ASSET_FILES "")
foreach(ASSET_FILE ${ASSET_FILES})
    string(REPLACE "${ASSET_SOURCE_DIR}/" "" RELATIVE_ASSET_FILE ${ASSET_FILE})
    set(DEST_FILE "${ASSET_OUTPUT_DIR}/${RELATIVE_ASSET_FILE}")
    get_filename_component(DEST_DIR ${DEST_FILE} DIRECTORY)

    add_custom_command(
        OUTPUT ${DEST_FILE}
        DEPENDS ${ASSET_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "${ASSET_FILE}" "${DEST_FILE}"
        COMMENT "Copying ${ASSET_FILE} to ${DEST_FILE}")
    list(APPEND COPIED_ASSET_FILES ${DEST_FILE})
endforeach()

add_library(Game SHARED "game.c"
                        "log.c"
                        "og_ds.c"
                        "platform.c"
                        "renderer.c"
                        "string_utils.c"
                        "hexgrid.h"
                        "hexgrid.c"
                        "resource.c"
                        "vulkan/vulkan.c"
                        "vulkan/buffer.c"
                        "vulkan/shader.c"
                        "vulkan/texture.c"
                        "vulkan/vma.cpp"
                        "vulkan/volk.c")

target_link_libraries(Game PRIVATE SDL3::SDL3 meshoptimizer)
target_include_directories(Game PRIVATE "extern" $ENV{VULKAN_SDK}/include)

add_custom_target(compile_shaders DEPENDS ${SPIRV_BINARY_FILES})
add_custom_target(copy_assets DEPENDS ${COPIED_ASSET_FILES})
add_dependencies(Game compile_shaders copy_assets)
