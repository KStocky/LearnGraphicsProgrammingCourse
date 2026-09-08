set(
    LGP_CH29_SCREEN_SPACE_REFLECTIONS_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ScreenSpaceReflectionContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ScreenSpaceReflectionContracts.hpp")
set(
    LGP_CH29_SCREEN_SPACE_REFLECTIONS_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ScreenSpaceReflectionShared.hlsli")
set(
    LGP_CH29_SCREEN_SPACE_REFLECTIONS_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ScreenSpaceReflectionLab.hlsl")
set(
    LGP_CH29_SCREEN_SPACE_REFLECTIONS_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ScreenSpaceReflectionLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ScreenSpaceReflectionShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ScreenSpaceReflectionLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ScreenSpaceReflectionLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch29_screen_space_reflection_contracts STATIC ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_CONTRACT_SOURCES})
add_library(LGP::Ch29ScreenSpaceReflectionContracts ALIAS ch29_screen_space_reflection_contracts)
target_link_libraries(
    ch29_screen_space_reflection_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch29_screen_space_reflection_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch29_screen_space_reflection_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ScreenSpaceReflections")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_CONTRACT_SOURCES})

add_library(ch29_screen_space_reflection_gpu_common STATIC ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_GPU_COMMON_SOURCES})
target_link_libraries(
    ch29_screen_space_reflection_gpu_common
    PUBLIC
        LGP::Ch29ScreenSpaceReflectionContracts
        LGP::Framework)
target_include_directories(ch29_screen_space_reflection_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch29_screen_space_reflection_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ScreenSpaceReflections")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_GPU_COMMON_SOURCES})

add_library(
    ch29_screen_space_reflection_starter_logic
    STATIC
    ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_STARTER_SOURCES})
target_link_libraries(ch29_screen_space_reflection_starter_logic PUBLIC ch29_screen_space_reflection_gpu_common)
target_include_directories(ch29_screen_space_reflection_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch29_screen_space_reflection_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ScreenSpaceReflections")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_STARTER_SOURCES})

add_library(
    ch29_screen_space_reflection_solution_logic
    STATIC
    ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_SOLUTION_SOURCES})
target_link_libraries(ch29_screen_space_reflection_solution_logic PUBLIC ch29_screen_space_reflection_gpu_common)
target_include_directories(ch29_screen_space_reflection_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch29_screen_space_reflection_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ScreenSpaceReflections")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH29_SCREEN_SPACE_REFLECTIONS_SOLUTION_SOURCES})

add_executable(Ch29_ScreenSpaceReflections_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch29_ScreenSpaceReflections_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ScreenSpaceReflections")
lgp_configure_framework_chapter_app(
    TARGET Ch29_ScreenSpaceReflections_Starter
    EXTRA_DEPENDENCIES ch29_screen_space_reflection_starter_logic)

add_executable(Ch29_ScreenSpaceReflections_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch29_ScreenSpaceReflections_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ScreenSpaceReflections")
lgp_configure_framework_chapter_app(
    TARGET Ch29_ScreenSpaceReflections_Solution
    EXTRA_DEPENDENCIES ch29_screen_space_reflection_solution_logic)

