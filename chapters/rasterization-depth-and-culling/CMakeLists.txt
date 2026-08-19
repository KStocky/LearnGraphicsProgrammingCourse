set(
    LGP_CH02_COMMON_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterTypes.hpp")

set(
    LGP_CH02_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/RasterizationMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/RasterizationMath.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")

set(
    LGP_CH02_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/RasterizationMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/RasterizationMath.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Rasterization.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

get_target_property(LGP_CH02_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH02_IMGUI_INCLUDE_DIRS 0 LGP_CH02_IMGUI_ROOT)
set(LGP_CH02_IMGUI_DX12_BACKEND "${LGP_CH02_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH02_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties("${LGP_CH02_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH02_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Rasterization.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch02_rasterization_starter_logic STATIC ${LGP_CH02_COMMON_HEADERS} ${LGP_CH02_STARTER_SOURCES})
target_link_libraries(ch02_rasterization_starter_logic PUBLIC LGP::Framework Microsoft::DirectXMath)
target_include_directories(ch02_rasterization_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch02_rasterization_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Rasterization")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH02_COMMON_HEADERS} ${LGP_CH02_STARTER_SOURCES})

add_library(
    ch02_rasterization_solution_logic
    STATIC
        ${LGP_CH02_COMMON_HEADERS}
        ${LGP_CH02_SOLUTION_SOURCES}
        "${LGP_CH02_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch02_rasterization_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(
    ch02_rasterization_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH02_IMGUI_ROOT}"
        "${LGP_CH02_IMGUI_ROOT}/backends")
target_compile_definitions(ch02_rasterization_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch02_rasterization_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Rasterization")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH02_COMMON_HEADERS} ${LGP_CH02_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH02_IMGUI_DX12_BACKEND}")

add_executable(Ch02_Rasterization_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch02_Rasterization_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Rasterization")
lgp_configure_framework_chapter_app(
    TARGET Ch02_Rasterization_Starter
    EXTRA_DEPENDENCIES ch02_rasterization_starter_logic)

add_executable(Ch02_Rasterization_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch02_Rasterization_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Rasterization")
lgp_configure_framework_chapter_app(
    TARGET Ch02_Rasterization_Solution
    EXTRA_DEPENDENCIES ch02_rasterization_solution_logic)

