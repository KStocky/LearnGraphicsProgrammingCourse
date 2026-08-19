set(
    LGP_CH06_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SurfaceFrame.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SurfaceFrame.hpp")

add_library(ch06_surface_frame_contracts STATIC ${LGP_CH06_COMMON_SOURCES})
add_library(LGP::Ch06SurfaceFrameContracts ALIAS ch06_surface_frame_contracts)
target_link_libraries(
    ch06_surface_frame_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(ch06_surface_frame_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch06_surface_frame_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SurfaceFrames")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH06_COMMON_SOURCES})

set(
    LGP_CH06_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SurfaceBaseline.hlsl")
set(
    LGP_CH06_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SurfaceBaseline.hlsl")

get_target_property(LGP_CH06_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH06_IMGUI_INCLUDE_DIRS 0 LGP_CH06_IMGUI_ROOT)
set(LGP_CH06_IMGUI_DX12_BACKEND "${LGP_CH06_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH06_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH06_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH06_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SurfaceBaseline.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SurfaceBaseline.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch06_surface_frames_starter_logic STATIC ${LGP_CH06_STARTER_SOURCES})
target_link_libraries(
    ch06_surface_frames_starter_logic
    PUBLIC
        LGP::Ch06SurfaceFrameContracts
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch06_surface_frames_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch06_surface_frames_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SurfaceFrames")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH06_STARTER_SOURCES})

add_library(
    ch06_surface_frames_solution_logic
    STATIC
        ${LGP_CH06_SOLUTION_SOURCES}
        "${LGP_CH06_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch06_surface_frames_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Ch06SurfaceFrameContracts
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(
    ch06_surface_frames_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH06_IMGUI_ROOT}"
        "${LGP_CH06_IMGUI_ROOT}/backends")
target_compile_definitions(ch06_surface_frames_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch06_surface_frames_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SurfaceFrames")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH06_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH06_IMGUI_DX12_BACKEND}")

add_executable(Ch06_SurfaceFrames_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch06_SurfaceFrames_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SurfaceFrames")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch06_SurfaceFrames_Starter
    EXTRA_DEPENDENCIES ch06_surface_frames_starter_logic)

add_executable(Ch06_SurfaceFrames_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch06_SurfaceFrames_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SurfaceFrames")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch06_SurfaceFrames_Solution
    EXTRA_DEPENDENCIES ch06_surface_frames_solution_logic)

