set(
    LGP_CH27_ENVIRONMENT_LIGHTING_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/EnvironmentLightingContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/EnvironmentLightingContracts.hpp")

add_library(
    ch27_environment_lighting_contracts
    STATIC
        ${LGP_CH27_ENVIRONMENT_LIGHTING_CONTRACT_SOURCES})
add_library(
    LGP::Ch27EnvironmentLightingContracts
    ALIAS ch27_environment_lighting_contracts)
target_link_libraries(
    ch27_environment_lighting_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch27_environment_lighting_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch27_environment_lighting_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/EnvironmentLighting")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH27_ENVIRONMENT_LIGHTING_CONTRACT_SOURCES})

set(
    LGP_CH27_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/EnvironmentLightingShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH27_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/EnvironmentLightingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH27_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/EnvironmentLightingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/EnvironmentLightingShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/EnvironmentLightingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/EnvironmentLightingLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch27_environment_lighting_gpu_common STATIC ${LGP_CH27_GPU_COMMON_SOURCES})
target_link_libraries(
    ch27_environment_lighting_gpu_common
    PUBLIC
        LGP::Ch27EnvironmentLightingContracts
        LGP::Framework)
target_include_directories(ch27_environment_lighting_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch27_environment_lighting_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/EnvironmentLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH27_GPU_COMMON_SOURCES})

add_library(ch27_environment_lighting_starter_logic STATIC ${LGP_CH27_STARTER_SOURCES})
target_link_libraries(ch27_environment_lighting_starter_logic PUBLIC ch27_environment_lighting_gpu_common)
target_include_directories(ch27_environment_lighting_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch27_environment_lighting_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/EnvironmentLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH27_STARTER_SOURCES})

add_library(ch27_environment_lighting_solution_logic STATIC ${LGP_CH27_SOLUTION_SOURCES})
target_link_libraries(ch27_environment_lighting_solution_logic PUBLIC ch27_environment_lighting_gpu_common)
target_include_directories(ch27_environment_lighting_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch27_environment_lighting_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/EnvironmentLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH27_SOLUTION_SOURCES})

add_executable(Ch27_EnvironmentLighting_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch27_EnvironmentLighting_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/EnvironmentLighting")
lgp_configure_framework_chapter_app(
    TARGET Ch27_EnvironmentLighting_Starter
    EXTRA_DEPENDENCIES ch27_environment_lighting_starter_logic)

add_executable(Ch27_EnvironmentLighting_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch27_EnvironmentLighting_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/EnvironmentLighting")
lgp_configure_framework_chapter_app(
    TARGET Ch27_EnvironmentLighting_Solution
    EXTRA_DEPENDENCIES ch27_environment_lighting_solution_logic)

