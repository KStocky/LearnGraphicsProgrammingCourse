set(
    LGP_CH07_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ChapterGeometry.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ShadowContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ShadowContracts.hpp")

add_library(ch07_shadow_contracts STATIC ${LGP_CH07_COMMON_SOURCES})
add_library(LGP::Ch07ShadowContracts ALIAS ch07_shadow_contracts)
target_link_libraries(ch07_shadow_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch07_shadow_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch07_shadow_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Shadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH07_COMMON_SOURCES})

set(
    LGP_CH07_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DirectionalBaseline.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH07_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DirectionalBaseline.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")
get_target_property(LGP_CH07_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH07_IMGUI_INCLUDE_DIRS 0 LGP_CH07_IMGUI_ROOT)
set(LGP_CH07_IMGUI_DX12_BACKEND "${LGP_CH07_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH07_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH07_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH07_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()
set(LGP_CH07_STARTER_STAGE 0)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DirectionalBaseline.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DirectionalBaseline.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(ch07_shadows_starter_logic STATIC ${LGP_CH07_STARTER_SOURCES})
target_link_libraries(
    ch07_shadows_starter_logic
    PUBLIC
        LGP::Ch07ShadowContracts
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch07_shadows_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(ch07_shadows_solution_logic STATIC ${LGP_CH07_SOLUTION_SOURCES} "${LGP_CH07_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch07_shadows_solution_logic
    PUBLIC
        DearImGui::DearImGui
        LGP::Ch07ShadowContracts
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(
    ch07_shadows_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH07_IMGUI_ROOT}"
        "${LGP_CH07_IMGUI_ROOT}/backends")
target_compile_definitions(ch07_shadows_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

foreach(target ch07_shadows_starter_logic ch07_shadows_solution_logic)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
            FOLDER "Chapters/Shadows")
endforeach()
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH07_STARTER_SOURCES} ${LGP_CH07_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH07_IMGUI_DX12_BACKEND}")

add_executable(Ch07_ShadowMaps_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
add_executable(Ch07_ShadowMaps_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
foreach(target Ch07_ShadowMaps_Starter Ch07_ShadowMaps_Solution)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
            FOLDER "Chapters/Shadows")
endforeach()
lgp_configure_framework_chapter_app(
    TARGET Ch07_ShadowMaps_Starter
    EXTRA_DEPENDENCIES ch07_shadows_starter_logic)
lgp_configure_framework_chapter_app(
    TARGET Ch07_ShadowMaps_Solution
    EXTRA_DEPENDENCIES ch07_shadows_solution_logic)

