set(
    LGP_CH26_SPHERICAL_HARMONICS_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SphericalHarmonicsContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SphericalHarmonicsContracts.hpp")

add_library(
    ch26_spherical_harmonics_contracts
    STATIC
        ${LGP_CH26_SPHERICAL_HARMONICS_CONTRACT_SOURCES})
add_library(
    LGP::Ch26SphericalHarmonicsContracts
    ALIAS ch26_spherical_harmonics_contracts)
target_link_libraries(
    ch26_spherical_harmonics_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch26_spherical_harmonics_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch26_spherical_harmonics_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SphericalHarmonicsForLighting")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH26_SPHERICAL_HARMONICS_CONTRACT_SOURCES})

set(
    LGP_CH26_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH26_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SphericalHarmonicsLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH26_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SphericalHarmonicsLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SphericalHarmonicsLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SphericalHarmonicsLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch26_spherical_harmonics_gpu_common STATIC ${LGP_CH26_GPU_COMMON_SOURCES})
target_link_libraries(
    ch26_spherical_harmonics_gpu_common
    PUBLIC
        LGP::Ch26SphericalHarmonicsContracts
        LGP::Framework)
target_include_directories(ch26_spherical_harmonics_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch26_spherical_harmonics_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SphericalHarmonicsForLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH26_GPU_COMMON_SOURCES})

add_library(ch26_spherical_harmonics_starter_logic STATIC ${LGP_CH26_STARTER_SOURCES})
target_link_libraries(
    ch26_spherical_harmonics_starter_logic
    PUBLIC
        ch26_spherical_harmonics_gpu_common)
target_include_directories(ch26_spherical_harmonics_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch26_spherical_harmonics_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SphericalHarmonicsForLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH26_STARTER_SOURCES})

add_library(ch26_spherical_harmonics_solution_logic STATIC ${LGP_CH26_SOLUTION_SOURCES})
target_link_libraries(
    ch26_spherical_harmonics_solution_logic
    PUBLIC
        ch26_spherical_harmonics_gpu_common)
target_include_directories(ch26_spherical_harmonics_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch26_spherical_harmonics_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SphericalHarmonicsForLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH26_SOLUTION_SOURCES})

add_executable(Ch26_SphericalHarmonics_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch26_SphericalHarmonics_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SphericalHarmonicsForLighting")
lgp_configure_framework_chapter_app(
    TARGET Ch26_SphericalHarmonics_Starter
    EXTRA_DEPENDENCIES ch26_spherical_harmonics_starter_logic)

add_executable(Ch26_SphericalHarmonics_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch26_SphericalHarmonics_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/SphericalHarmonicsForLighting")
lgp_configure_framework_chapter_app(
    TARGET Ch26_SphericalHarmonics_Solution
    EXTRA_DEPENDENCIES ch26_spherical_harmonics_solution_logic)

