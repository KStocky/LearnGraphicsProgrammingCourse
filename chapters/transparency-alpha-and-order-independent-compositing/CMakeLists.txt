set(
    LGP_CH32_TRANSPARENCY_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransparencyContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransparencyContracts.hpp")
set(
    LGP_CH32_TRANSPARENCY_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransparencyShared.hlsli")
set(
    LGP_CH32_TRANSPARENCY_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TransparencyLab.hlsl")
set(
    LGP_CH32_TRANSPARENCY_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TransparencyLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransparencyShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TransparencyLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TransparencyLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch32_transparency_contracts STATIC ${LGP_CH32_TRANSPARENCY_CONTRACT_SOURCES})
add_library(LGP::Ch32TransparencyContracts ALIAS ch32_transparency_contracts)
target_link_libraries(
    ch32_transparency_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch32_transparency_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch32_transparency_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Transparency")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH32_TRANSPARENCY_CONTRACT_SOURCES})

add_library(ch32_transparency_gpu_common STATIC ${LGP_CH32_TRANSPARENCY_GPU_COMMON_SOURCES})
target_link_libraries(
    ch32_transparency_gpu_common
    PUBLIC
        LGP::Ch32TransparencyContracts
        LGP::Framework)
target_include_directories(ch32_transparency_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch32_transparency_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Transparency")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH32_TRANSPARENCY_GPU_COMMON_SOURCES})

add_library(ch32_transparency_starter_logic STATIC ${LGP_CH32_TRANSPARENCY_STARTER_SOURCES})
target_link_libraries(ch32_transparency_starter_logic PUBLIC ch32_transparency_gpu_common)
target_include_directories(ch32_transparency_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch32_transparency_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Transparency")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH32_TRANSPARENCY_STARTER_SOURCES})

add_library(ch32_transparency_solution_logic STATIC ${LGP_CH32_TRANSPARENCY_SOLUTION_SOURCES})
target_link_libraries(ch32_transparency_solution_logic PUBLIC ch32_transparency_gpu_common)
target_include_directories(ch32_transparency_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch32_transparency_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Transparency")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH32_TRANSPARENCY_SOLUTION_SOURCES})

add_executable(Ch32_Transparency_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch32_Transparency_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Transparency")
lgp_configure_framework_chapter_app(
    TARGET Ch32_Transparency_Starter
    EXTRA_DEPENDENCIES ch32_transparency_starter_logic)

add_executable(Ch32_Transparency_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch32_Transparency_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Transparency")
lgp_configure_framework_chapter_app(
    TARGET Ch32_Transparency_Solution
    EXTRA_DEPENDENCIES ch32_transparency_solution_logic)

