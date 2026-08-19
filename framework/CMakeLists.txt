set(
    LGP_FRAMEWORK_PUBLIC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/application.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/barriers.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/buffer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/command_line.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/descriptors.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/device_resources.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/error.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/input.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/pix.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/shader_compiler.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lgp/framework/timer.hpp")

set(
    LGP_FRAMEWORK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/barriers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/command_line.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/descriptors.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/device_resources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/error.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pix.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shader_compiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/timer.cpp")

set(
    LGP_FRAMEWORK_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/LgpShaderCommon.hlsli")

add_library(
    lgp_framework
    STATIC
        ${LGP_FRAMEWORK_PUBLIC_HEADERS}
        ${LGP_FRAMEWORK_SOURCES}
        ${LGP_FRAMEWORK_SHADER_SOURCES})
add_library(LGP::Framework ALIAS lgp_framework)

target_link_libraries(
    lgp_framework
    PUBLIC
        LGP::CompilerFlags
        Microsoft::Direct3D12AgilitySDK
        Microsoft::DirectXMath
        Microsoft::DXC
        Microsoft::WinPixEventRuntime
        d3d12
        dxgi
        dxguid
        user32
        shell32)

target_include_directories(
    lgp_framework
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_sources(lgp_framework PUBLIC FILE_SET HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include" FILES ${LGP_FRAMEWORK_PUBLIC_HEADERS})

set_target_properties(
    lgp_framework
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Framework"
        LGP_FRAMEWORK_SHADER_INCLUDE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_FRAMEWORK_PUBLIC_HEADERS} ${LGP_FRAMEWORK_SOURCES} ${LGP_FRAMEWORK_SHADER_SOURCES})

function(lgp_get_framework_shader_include_directory out_var)
    if(NOT TARGET lgp_framework)
        message(FATAL_ERROR "lgp_framework must exist before querying its shader include directory.")
    endif()

    get_target_property(_lgp_shader_include_dir lgp_framework LGP_FRAMEWORK_SHADER_INCLUDE_DIRECTORY)
    if(NOT _lgp_shader_include_dir OR _lgp_shader_include_dir STREQUAL "_lgp_shader_include_dir-NOTFOUND")
        message(FATAL_ERROR "lgp_framework does not declare a shader include directory.")
    endif()

    set("${out_var}" "${_lgp_shader_include_dir}" PARENT_SCOPE)
endfunction()

function(lgp_configure_framework_runtime_target)
    set(options ENABLE_IMGUI)
    set(oneValueArgs TARGET)
    set(multiValueArgs EXTRA_DEPENDENCIES)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "lgp_configure_framework_runtime_target requires TARGET.")
    endif()

    if(NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR "Framework runtime target ${ARG_TARGET} does not exist.")
    endif()

    target_link_libraries("${ARG_TARGET}" PRIVATE LGP::Framework ${ARG_EXTRA_DEPENDENCIES})

    if(ARG_ENABLE_IMGUI)
        if(NOT LGP_ENABLE_IMGUI)
            message(FATAL_ERROR "ENABLE_IMGUI was requested but LGP_ENABLE_IMGUI is OFF.")
        endif()

        if(NOT TARGET DearImGui::DearImGui)
            message(FATAL_ERROR "DearImGui::DearImGui is unavailable.")
        endif()

        target_link_libraries("${ARG_TARGET}" PRIVATE DearImGui::DearImGui)
    endif()

    lgp_enable_d3d12_agility_sdk(TARGET "${ARG_TARGET}")
    lgp_stage_runtime_dependencies(
        TARGET "${ARG_TARGET}"
        DEPENDENCIES
            Microsoft::DXC
            Microsoft::WinPixEventRuntime)
endfunction()

function(lgp_configure_framework_chapter_app)
    set(options ENABLE_IMGUI)
    set(oneValueArgs TARGET)
    set(multiValueArgs EXTRA_DEPENDENCIES)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "lgp_configure_framework_chapter_app requires TARGET.")
    endif()

    set(_lgp_enable_imgui)
    if(ARG_ENABLE_IMGUI)
        set(_lgp_enable_imgui ENABLE_IMGUI)
    endif()

    lgp_configure_framework_runtime_target(
        TARGET "${ARG_TARGET}"
        ${_lgp_enable_imgui}
        EXTRA_DEPENDENCIES ${ARG_EXTRA_DEPENDENCIES})
endfunction()
