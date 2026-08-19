set(
    LGP_CH08_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PassGraph.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PassGraph.hpp")

set(
    LGP_CH08_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/PassGraphLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")

set(
    LGP_CH08_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/PassGraphLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

get_target_property(LGP_CH08_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH08_IMGUI_INCLUDE_DIRS 0 LGP_CH08_IMGUI_ROOT)
set(LGP_CH08_IMGUI_DX12_BACKEND "${LGP_CH08_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH08_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH08_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH08_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/PassGraphLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/PassGraphLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch08_frame_graph_contracts STATIC ${LGP_CH08_COMMON_SOURCES})
add_library(LGP::Ch08FrameGraphContracts ALIAS ch08_frame_graph_contracts)
target_link_libraries(
    ch08_frame_graph_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch08_frame_graph_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common"
        "${PROJECT_SOURCE_DIR}/framework/include")
set_target_properties(
    ch08_frame_graph_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/FrameGraph")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH08_COMMON_SOURCES})

add_library(ch08_pass_graph_starter_logic STATIC ${LGP_CH08_STARTER_SOURCES})
target_link_libraries(
    ch08_pass_graph_starter_logic
    PUBLIC
        LGP::Ch08FrameGraphContracts
        LGP::Framework)
target_include_directories(ch08_pass_graph_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch08_pass_graph_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/FrameGraph")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH08_STARTER_SOURCES})

add_library(
    ch08_pass_graph_solution_logic
    STATIC
        ${LGP_CH08_SOLUTION_SOURCES}
        "${LGP_CH08_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch08_pass_graph_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Ch08FrameGraphContracts
        LGP::Framework)
target_include_directories(
    ch08_pass_graph_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH08_IMGUI_ROOT}"
        "${LGP_CH08_IMGUI_ROOT}/backends")
target_compile_definitions(ch08_pass_graph_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch08_pass_graph_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/FrameGraph")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH08_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH08_IMGUI_DX12_BACKEND}")

add_executable(Ch08_PassGraph_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch08_PassGraph_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/FrameGraph")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch08_PassGraph_Starter
    EXTRA_DEPENDENCIES ch08_pass_graph_starter_logic)

add_executable(Ch08_PassGraph_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch08_PassGraph_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/FrameGraph")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch08_PassGraph_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch08_pass_graph_solution_logic)

