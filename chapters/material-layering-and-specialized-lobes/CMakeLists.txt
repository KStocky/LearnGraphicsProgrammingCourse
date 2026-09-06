# Chapter 23 - Material Layering and Specialized BRDF Lobes.
#
# This chapter ships the CPU mathematical contract library and its Catch2 tests,
# plus the paired runnable D3D12/HLSL lab: a Starter isotropic baseline, a Solution
# material-lobe lab, shared chapter-owned GPU support, and deterministic WARP tests.

set(
    LGP_CH23_MATERIAL_LAYERING_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MaterialLayering.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MaterialLayering.hpp")

add_library(ch23_material_layering_contracts STATIC ${LGP_CH23_MATERIAL_LAYERING_CONTRACT_SOURCES})
add_library(LGP::Ch23MaterialLayeringContracts ALIAS ch23_material_layering_contracts)
target_link_libraries(
    ch23_material_layering_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(ch23_material_layering_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch23_material_layering_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MaterialLayering")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH23_MATERIAL_LAYERING_CONTRACT_SOURCES})

set(
    LGP_CH23_MATERIAL_LAYERING_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MaterialLab.hlsli")
set(
    LGP_CH23_MATERIAL_LAYERING_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MaterialLab.hlsl")
set(
    LGP_CH23_MATERIAL_LAYERING_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MaterialLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MaterialLab.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MaterialLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MaterialLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch23_material_layering_common STATIC ${LGP_CH23_MATERIAL_LAYERING_COMMON_SOURCES})
target_link_libraries(
    ch23_material_layering_common
    PUBLIC
        LGP::Ch23MaterialLayeringContracts
        LGP::Framework)
target_include_directories(ch23_material_layering_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch23_material_layering_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MaterialLayering")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH23_MATERIAL_LAYERING_COMMON_SOURCES})

add_library(ch23_material_layering_starter_logic STATIC ${LGP_CH23_MATERIAL_LAYERING_STARTER_SOURCES})
target_link_libraries(ch23_material_layering_starter_logic PUBLIC ch23_material_layering_common)
target_include_directories(ch23_material_layering_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch23_material_layering_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MaterialLayering")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH23_MATERIAL_LAYERING_STARTER_SOURCES})

add_library(ch23_material_layering_solution_logic STATIC ${LGP_CH23_MATERIAL_LAYERING_SOLUTION_SOURCES})
target_link_libraries(ch23_material_layering_solution_logic PUBLIC ch23_material_layering_common)
target_include_directories(ch23_material_layering_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch23_material_layering_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MaterialLayering")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH23_MATERIAL_LAYERING_SOLUTION_SOURCES})

add_executable(Ch23_MaterialLayering_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch23_MaterialLayering_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MaterialLayering")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch23_MaterialLayering_Starter
    EXTRA_DEPENDENCIES ch23_material_layering_starter_logic)

add_executable(Ch23_MaterialLayering_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch23_MaterialLayering_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MaterialLayering")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch23_MaterialLayering_Solution
    EXTRA_DEPENDENCIES ch23_material_layering_solution_logic)

