set(
    LGP_CH03_COMMON_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterTypes.hpp")

set(
    LGP_CH03_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TextureSamplingMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TextureSamplingMath.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")

set(
    LGP_CH03_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TextureSamplingMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TextureSamplingMath.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TextureSampling.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TextureSampling.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

get_target_property(LGP_CH03_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH03_IMGUI_INCLUDE_DIRS 0 LGP_CH03_IMGUI_ROOT)
set(LGP_CH03_IMGUI_DX12_BACKEND "${LGP_CH03_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH03_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties("${LGP_CH03_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH03_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

add_library(ch03_texture_sampling_starter_logic STATIC ${LGP_CH03_COMMON_HEADERS} ${LGP_CH03_STARTER_SOURCES})
target_link_libraries(ch03_texture_sampling_starter_logic PUBLIC LGP::Framework Microsoft::DirectXMath)
target_include_directories(ch03_texture_sampling_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch03_texture_sampling_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TextureSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH03_COMMON_HEADERS} ${LGP_CH03_STARTER_SOURCES})

add_library(
    ch03_texture_sampling_solution_logic
    STATIC
        ${LGP_CH03_COMMON_HEADERS}
        ${LGP_CH03_SOLUTION_SOURCES}
        "${LGP_CH03_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch03_texture_sampling_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(
    ch03_texture_sampling_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH03_IMGUI_ROOT}"
        "${LGP_CH03_IMGUI_ROOT}/backends")
target_compile_definitions(ch03_texture_sampling_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch03_texture_sampling_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TextureSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH03_COMMON_HEADERS} ${LGP_CH03_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH03_IMGUI_DX12_BACKEND}")

add_executable(Ch03_TextureSampling_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch03_TextureSampling_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TextureSampling")
lgp_configure_framework_chapter_app(
    TARGET Ch03_TextureSampling_Starter
    EXTRA_DEPENDENCIES ch03_texture_sampling_starter_logic)

add_executable(Ch03_TextureSampling_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch03_TextureSampling_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TextureSampling")
lgp_configure_framework_chapter_app(
    TARGET Ch03_TextureSampling_Solution
    EXTRA_DEPENDENCIES ch03_texture_sampling_solution_logic)

