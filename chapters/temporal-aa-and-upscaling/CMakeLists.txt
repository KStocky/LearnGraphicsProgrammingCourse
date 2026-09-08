set(
    LGP_CH28_TEMPORAL_AA_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TemporalAaContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TemporalAaContracts.hpp")
set(
    LGP_CH28_TEMPORAL_AA_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TemporalAaShared.hlsli")
set(
    LGP_CH28_TEMPORAL_AA_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TemporalAaLab.hlsl")
set(
    LGP_CH28_TEMPORAL_AA_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TemporalAaLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TemporalAaShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TemporalAaLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TemporalAaLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch28_temporal_aa_contracts STATIC ${LGP_CH28_TEMPORAL_AA_CONTRACT_SOURCES})
add_library(LGP::Ch28TemporalAaContracts ALIAS ch28_temporal_aa_contracts)
target_link_libraries(
    ch28_temporal_aa_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(ch28_temporal_aa_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch28_temporal_aa_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TemporalAA")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH28_TEMPORAL_AA_CONTRACT_SOURCES})

add_library(ch28_temporal_aa_gpu_common STATIC ${LGP_CH28_TEMPORAL_AA_GPU_COMMON_SOURCES})
target_link_libraries(
    ch28_temporal_aa_gpu_common
    PUBLIC
        LGP::Ch28TemporalAaContracts
        LGP::Framework)
target_include_directories(ch28_temporal_aa_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch28_temporal_aa_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TemporalAA")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH28_TEMPORAL_AA_GPU_COMMON_SOURCES})

add_library(ch28_temporal_aa_starter_logic STATIC ${LGP_CH28_TEMPORAL_AA_STARTER_SOURCES})
target_link_libraries(ch28_temporal_aa_starter_logic PUBLIC ch28_temporal_aa_gpu_common)
target_include_directories(ch28_temporal_aa_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch28_temporal_aa_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TemporalAA")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH28_TEMPORAL_AA_STARTER_SOURCES})

add_library(ch28_temporal_aa_solution_logic STATIC ${LGP_CH28_TEMPORAL_AA_SOLUTION_SOURCES})
target_link_libraries(ch28_temporal_aa_solution_logic PUBLIC ch28_temporal_aa_gpu_common)
target_include_directories(ch28_temporal_aa_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch28_temporal_aa_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TemporalAA")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH28_TEMPORAL_AA_SOLUTION_SOURCES})

add_executable(Ch28_TemporalAA_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch28_TemporalAA_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TemporalAA")
lgp_configure_framework_chapter_app(
    TARGET Ch28_TemporalAA_Starter
    EXTRA_DEPENDENCIES ch28_temporal_aa_starter_logic)

add_executable(Ch28_TemporalAA_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch28_TemporalAA_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TemporalAA")
lgp_configure_framework_chapter_app(
    TARGET Ch28_TemporalAA_Solution
    EXTRA_DEPENDENCIES ch28_temporal_aa_solution_logic)

