set(
    LGP_CH05_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/LightBrdfMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/LightBrdfMath.hpp")

add_library(ch05_lighting_contracts STATIC ${LGP_CH05_COMMON_SOURCES})
add_library(LGP::Ch05LightingContracts ALIAS ch05_lighting_contracts)
target_link_libraries(
    ch05_lighting_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(ch05_lighting_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch05_lighting_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/RadiometryLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH05_COMMON_SOURCES})

set(
    LGP_CH05_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")

set(
    LGP_CH05_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DirectLighting.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

get_target_property(LGP_CH05_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH05_IMGUI_INCLUDE_DIRS 0 LGP_CH05_IMGUI_ROOT)
set(LGP_CH05_IMGUI_DX12_BACKEND "${LGP_CH05_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH05_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH05_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH05_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DirectLighting.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch05_lighting_starter_logic STATIC ${LGP_CH05_STARTER_SOURCES})
target_link_libraries(
    ch05_lighting_starter_logic
    PUBLIC
        LGP::Ch05LightingContracts
        LGP::Framework)
target_include_directories(ch05_lighting_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch05_lighting_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/RadiometryLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH05_STARTER_SOURCES})

add_library(
    ch05_lighting_solution_logic
    STATIC
        ${LGP_CH05_SOLUTION_SOURCES}
        "${LGP_CH05_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch05_lighting_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Ch05LightingContracts
        LGP::Framework)
target_include_directories(
    ch05_lighting_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH05_IMGUI_ROOT}"
        "${LGP_CH05_IMGUI_ROOT}/backends")
target_compile_definitions(ch05_lighting_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch05_lighting_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/RadiometryLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH05_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH05_IMGUI_DX12_BACKEND}")

add_executable(Ch05_RadiometryLighting_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch05_RadiometryLighting_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/RadiometryLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch05_RadiometryLighting_Starter
    EXTRA_DEPENDENCIES ch05_lighting_starter_logic)

add_executable(Ch05_RadiometryLighting_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch05_RadiometryLighting_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/RadiometryLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch05_RadiometryLighting_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch05_lighting_solution_logic)

