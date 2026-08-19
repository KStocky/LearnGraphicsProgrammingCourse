set(
    LGP_CH04_COMMON_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterTypes.hpp")

set(
    LGP_CH04_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ColorPipelineMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ColorPipelineMath.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")

set(
    LGP_CH04_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ColorPipelineMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ColorPipelineMath.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ColorPipeline.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

get_target_property(LGP_CH04_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH04_IMGUI_INCLUDE_DIRS 0 LGP_CH04_IMGUI_ROOT)
set(LGP_CH04_IMGUI_DX12_BACKEND "${LGP_CH04_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH04_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties("${LGP_CH04_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH04_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ColorPipeline.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch04_color_pipeline_starter_logic STATIC ${LGP_CH04_COMMON_HEADERS} ${LGP_CH04_STARTER_SOURCES})
target_link_libraries(ch04_color_pipeline_starter_logic PUBLIC LGP::Framework)
target_include_directories(ch04_color_pipeline_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch04_color_pipeline_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ColorPipeline")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH04_COMMON_HEADERS} ${LGP_CH04_STARTER_SOURCES})

add_library(
    ch04_color_pipeline_solution_logic
    STATIC
        ${LGP_CH04_COMMON_HEADERS}
        ${LGP_CH04_SOLUTION_SOURCES}
        "${LGP_CH04_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch04_color_pipeline_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Framework)
target_include_directories(
    ch04_color_pipeline_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH04_IMGUI_ROOT}"
        "${LGP_CH04_IMGUI_ROOT}/backends")
target_compile_definitions(ch04_color_pipeline_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch04_color_pipeline_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ColorPipeline")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH04_COMMON_HEADERS} ${LGP_CH04_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH04_IMGUI_DX12_BACKEND}")

add_executable(Ch04_ColorPipeline_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch04_ColorPipeline_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ColorPipeline")
lgp_configure_framework_chapter_app(
    TARGET Ch04_ColorPipeline_Starter
    EXTRA_DEPENDENCIES ch04_color_pipeline_starter_logic)

add_executable(Ch04_ColorPipeline_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch04_ColorPipeline_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ColorPipeline")
lgp_configure_framework_chapter_app(
    TARGET Ch04_ColorPipeline_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch04_color_pipeline_solution_logic)

