set(
    LGP_CH01_COMMON_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterLogic.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterRenderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterTypes.hpp")

set(
    LGP_CH01_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterTypes.cpp")

set(
    LGP_CH01_STARTER_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Camera.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DemoLogic.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MathTransforms.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Scene.hpp")

set(
    LGP_CH01_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DemoLogic.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MathTransforms.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Visualizer.hlsl")

set(
    LGP_CH01_SOLUTION_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Camera.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DemoLogic.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MathTransforms.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Scene.hpp")

set(
    LGP_CH01_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DemoLogic.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MathTransforms.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Visualizer.hlsl")


get_target_property(LGP_CH01_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH01_IMGUI_INCLUDE_DIRS 0 LGP_CH01_IMGUI_ROOT)
set(LGP_CH01_IMGUI_DX12_BACKEND "${LGP_CH01_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH01_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties("${LGP_CH01_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH01_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Visualizer.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Visualizer.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(
    ch01_graphics_math_common
    STATIC
        ${LGP_CH01_COMMON_HEADERS}
        ${LGP_CH01_COMMON_SOURCES}
        "${LGP_CH01_IMGUI_DX12_BACKEND}")

target_link_libraries(
    ch01_graphics_math_common
    PUBLIC
        DearImGui::DearImGui
        LGP::Framework
        Microsoft::DirectXMath)

target_include_directories(
    ch01_graphics_math_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH01_IMGUI_ROOT}"
        "${LGP_CH01_IMGUI_ROOT}/backends")

target_compile_definitions(ch01_graphics_math_common PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

set_target_properties(
    ch01_graphics_math_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GraphicsMath")

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${LGP_CH01_COMMON_HEADERS}
        ${LGP_CH01_COMMON_SOURCES})

source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH01_IMGUI_DX12_BACKEND}")

add_library(
    ch01_graphics_math_starter_logic
    STATIC
        ${LGP_CH01_STARTER_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Camera.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DemoLogic.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MathTransforms.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Scene.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Visualizer.hlsl")

target_link_libraries(
    ch01_graphics_math_starter_logic
    PUBLIC
        ch01_graphics_math_common
        LGP::Framework
        Microsoft::DirectXMath)

target_include_directories(ch01_graphics_math_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(
    ch01_graphics_math_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GraphicsMath")

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${LGP_CH01_STARTER_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Camera.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DemoLogic.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MathTransforms.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Scene.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Visualizer.hlsl")

add_library(
    ch01_graphics_math_solution_logic
    STATIC
        ${LGP_CH01_SOLUTION_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Camera.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DemoLogic.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MathTransforms.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Scene.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Visualizer.hlsl")

target_link_libraries(
    ch01_graphics_math_solution_logic
    PUBLIC
        ch01_graphics_math_common
        LGP::Framework
        Microsoft::DirectXMath)

target_include_directories(ch01_graphics_math_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(
    ch01_graphics_math_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GraphicsMath")

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${LGP_CH01_SOLUTION_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Camera.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DemoLogic.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MathTransforms.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Scene.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Visualizer.hlsl")

add_executable(Ch01_GraphicsMath_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch01_GraphicsMath_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GraphicsMath")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch01_GraphicsMath_Starter
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES
        ch01_graphics_math_common
        ch01_graphics_math_starter_logic)

add_executable(Ch01_GraphicsMath_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch01_GraphicsMath_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GraphicsMath")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch01_GraphicsMath_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES
        ch01_graphics_math_common
        ch01_graphics_math_solution_logic)

