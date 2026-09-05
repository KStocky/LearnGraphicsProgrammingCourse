set(
    LGP_CH19_GPU_PROFILING_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ProfilingContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ProfilingContracts.hpp")
set(
    LGP_CH19_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ProfilingLab.hlsli")
set(
    LGP_CH19_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ProfilingLab.hlsl")
set(
    LGP_CH19_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ProfilingLab.hlsl")

add_library(ch19_gpu_profiling_contracts STATIC ${LGP_CH19_GPU_PROFILING_CONTRACT_SOURCES})
add_library(LGP::Ch19GpuProfilingContracts ALIAS ch19_gpu_profiling_contracts)
target_link_libraries(
    ch19_gpu_profiling_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch19_gpu_profiling_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch19_gpu_profiling_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuProfiling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH19_GPU_PROFILING_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ProfilingLab.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ProfilingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ProfilingLab.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(ch19_gpu_profiling_common STATIC ${LGP_CH19_GPU_COMMON_SOURCES})
target_link_libraries(ch19_gpu_profiling_common PUBLIC LGP::Ch19GpuProfilingContracts LGP::Framework)
target_include_directories(ch19_gpu_profiling_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch19_gpu_profiling_common PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                                                          FOLDER "Chapters/GpuProfiling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH19_GPU_COMMON_SOURCES})

add_library(ch19_gpu_profiling_starter_logic STATIC ${LGP_CH19_STARTER_SOURCES})
target_link_libraries(ch19_gpu_profiling_starter_logic PUBLIC ch19_gpu_profiling_common)
target_include_directories(ch19_gpu_profiling_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch19_gpu_profiling_starter_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
                                                                  CXX_EXTENSIONS NO FOLDER "Chapters/GpuProfiling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH19_STARTER_SOURCES})

add_library(ch19_gpu_profiling_solution_logic STATIC ${LGP_CH19_SOLUTION_SOURCES})
target_link_libraries(ch19_gpu_profiling_solution_logic PUBLIC ch19_gpu_profiling_common)
target_include_directories(ch19_gpu_profiling_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch19_gpu_profiling_solution_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
                                                                   CXX_EXTENSIONS NO FOLDER "Chapters/GpuProfiling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH19_SOLUTION_SOURCES})

add_executable(Ch19_GpuProfiling_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(Ch19_GpuProfiling_Starter PROPERTIES FOLDER "Chapters/GpuProfiling")
lgp_configure_framework_chapter_app(
    TARGET Ch19_GpuProfiling_Starter
    EXTRA_DEPENDENCIES ch19_gpu_profiling_starter_logic)

add_executable(Ch19_GpuProfiling_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(Ch19_GpuProfiling_Solution PROPERTIES FOLDER "Chapters/GpuProfiling")
lgp_configure_framework_chapter_app(
    TARGET Ch19_GpuProfiling_Solution
    EXTRA_DEPENDENCIES ch19_gpu_profiling_solution_logic)

