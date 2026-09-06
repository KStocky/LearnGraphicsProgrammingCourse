# Chapter 24 - Cascaded Shadow Maps.
#
# This chapter ships the CPU mathematical contract library and its Catch2 tests,
# plus the paired runnable D3D12/HLSL lab: a Starter single-map directional shadow
# baseline, a Solution four-cascade Texture2DArray lab, shared chapter-owned GPU
# support, and deterministic WARP tests.

set(
    LGP_CH24_CASCADED_SHADOWS_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/CascadedShadowContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/CascadedShadowContracts.hpp")

add_library(ch24_cascaded_shadows_contracts STATIC ${LGP_CH24_CASCADED_SHADOWS_CONTRACT_SOURCES})
add_library(LGP::Ch24CascadedShadowsContracts ALIAS ch24_cascaded_shadows_contracts)
target_link_libraries(
    ch24_cascaded_shadows_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(ch24_cascaded_shadows_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch24_cascaded_shadows_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/CascadedShadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH24_CASCADED_SHADOWS_CONTRACT_SOURCES})

set(
    LGP_CH24_CASCADED_SHADOWS_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/CascadedShadowLab.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/CascadedShadowLab.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/CascadedShadowLab.hlsli")
set(
    LGP_CH24_CASCADED_SHADOWS_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/CascadedShadowLab.hlsl")
set(
    LGP_CH24_CASCADED_SHADOWS_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/CascadedShadowLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/CascadedShadowLab.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/CascadedShadowLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/CascadedShadowLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch24_cascaded_shadows_common STATIC ${LGP_CH24_CASCADED_SHADOWS_COMMON_SOURCES})
target_link_libraries(
    ch24_cascaded_shadows_common
    PUBLIC
        LGP::Ch24CascadedShadowsContracts
        LGP::Framework)
target_include_directories(ch24_cascaded_shadows_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch24_cascaded_shadows_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/CascadedShadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH24_CASCADED_SHADOWS_COMMON_SOURCES})

add_library(ch24_cascaded_shadows_starter_logic STATIC ${LGP_CH24_CASCADED_SHADOWS_STARTER_SOURCES})
target_link_libraries(ch24_cascaded_shadows_starter_logic PUBLIC ch24_cascaded_shadows_common)
target_include_directories(ch24_cascaded_shadows_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch24_cascaded_shadows_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/CascadedShadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH24_CASCADED_SHADOWS_STARTER_SOURCES})

add_library(ch24_cascaded_shadows_solution_logic STATIC ${LGP_CH24_CASCADED_SHADOWS_SOLUTION_SOURCES})
target_link_libraries(ch24_cascaded_shadows_solution_logic PUBLIC ch24_cascaded_shadows_common)
target_include_directories(ch24_cascaded_shadows_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch24_cascaded_shadows_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/CascadedShadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH24_CASCADED_SHADOWS_SOLUTION_SOURCES})

add_executable(Ch24_CascadedShadows_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch24_CascadedShadows_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/CascadedShadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch24_CascadedShadows_Starter
    EXTRA_DEPENDENCIES ch24_cascaded_shadows_starter_logic)

add_executable(Ch24_CascadedShadows_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch24_CascadedShadows_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/CascadedShadows")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch24_CascadedShadows_Solution
    EXTRA_DEPENDENCIES ch24_cascaded_shadows_solution_logic)

