set(
    LGP_CH34_PARTICLE_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ParticleContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ParticleContracts.hpp")
set(
    LGP_CH34_PARTICLE_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH34_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ParticleLab.hlsl")
set(
    LGP_CH34_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ParticleLab.hlsl")

add_library(ch34_particle_contracts STATIC ${LGP_CH34_PARTICLE_CONTRACT_SOURCES})
add_library(LGP::Ch34ParticleContracts ALIAS ch34_particle_contracts)
target_link_libraries(
    ch34_particle_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch34_particle_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch34_particle_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuParticleSystems")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH34_PARTICLE_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ParticleLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ParticleLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch34_particle_gpu_common STATIC ${LGP_CH34_PARTICLE_GPU_COMMON_SOURCES})
add_library(LGP::Ch34ParticleGpuCommon ALIAS ch34_particle_gpu_common)
target_link_libraries(
    ch34_particle_gpu_common
    PUBLIC
        LGP::Ch34ParticleContracts
        LGP::Framework)
target_include_directories(ch34_particle_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch34_particle_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuParticleSystems")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH34_PARTICLE_GPU_COMMON_SOURCES})

add_library(ch34_particle_starter_logic STATIC ${LGP_CH34_STARTER_SOURCES})
target_link_libraries(
    ch34_particle_starter_logic
    PUBLIC
        ch34_particle_gpu_common
        LGP::Framework)
target_include_directories(ch34_particle_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch34_particle_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuParticleSystems")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH34_STARTER_SOURCES})

add_library(ch34_particle_solution_logic STATIC ${LGP_CH34_SOLUTION_SOURCES})
target_link_libraries(
    ch34_particle_solution_logic
    PUBLIC
        ch34_particle_gpu_common
        LGP::Framework)
target_include_directories(ch34_particle_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch34_particle_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuParticleSystems")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH34_SOLUTION_SOURCES})

add_executable(Ch34_GpuParticleSystems_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch34_GpuParticleSystems_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuParticleSystems")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch34_GpuParticleSystems_Starter
    EXTRA_DEPENDENCIES ch34_particle_starter_logic)

add_executable(Ch34_GpuParticleSystems_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch34_GpuParticleSystems_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuParticleSystems")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch34_GpuParticleSystems_Solution
    EXTRA_DEPENDENCIES ch34_particle_solution_logic)

