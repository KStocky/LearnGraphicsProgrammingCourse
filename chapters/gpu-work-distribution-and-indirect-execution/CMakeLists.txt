set(
    LGP_CH13_WORK_DISTRIBUTION_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/WorkDistribution.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/WorkDistribution.hpp")
set(
    LGP_CH13_WORK_DISTRIBUTION_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH13_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/WorkDistributionLab.hlsl")
set(
    LGP_CH13_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/WorkDistributionLab.hlsl")

add_library(ch13_work_distribution_contracts STATIC ${LGP_CH13_WORK_DISTRIBUTION_CONTRACT_SOURCES})
add_library(LGP::Ch13WorkDistributionContracts ALIAS ch13_work_distribution_contracts)
target_link_libraries(ch13_work_distribution_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(
    ch13_work_distribution_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch13_work_distribution_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkDistribution")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH13_WORK_DISTRIBUTION_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/WorkDistributionLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/WorkDistributionLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch13_work_distribution_gpu_common STATIC ${LGP_CH13_WORK_DISTRIBUTION_GPU_COMMON_SOURCES})
target_link_libraries(
    ch13_work_distribution_gpu_common
    PUBLIC
        LGP::Ch13WorkDistributionContracts
        LGP::Framework)
target_include_directories(ch13_work_distribution_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch13_work_distribution_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkDistribution")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH13_WORK_DISTRIBUTION_GPU_COMMON_SOURCES})

add_library(ch13_work_distribution_starter_logic STATIC ${LGP_CH13_STARTER_SOURCES})
target_link_libraries(
    ch13_work_distribution_starter_logic
    PUBLIC
        ch13_work_distribution_gpu_common
        LGP::Framework)
target_include_directories(ch13_work_distribution_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch13_work_distribution_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkDistribution")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH13_STARTER_SOURCES})

add_library(ch13_work_distribution_solution_logic STATIC ${LGP_CH13_SOLUTION_SOURCES})
target_link_libraries(
    ch13_work_distribution_solution_logic
    PUBLIC
        ch13_work_distribution_gpu_common
        LGP::Framework)
target_include_directories(ch13_work_distribution_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch13_work_distribution_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkDistribution")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH13_SOLUTION_SOURCES})

add_executable(Ch13_GpuWorkDistribution_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch13_GpuWorkDistribution_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkDistribution")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch13_GpuWorkDistribution_Starter
    EXTRA_DEPENDENCIES ch13_work_distribution_starter_logic)

add_executable(Ch13_GpuWorkDistribution_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch13_GpuWorkDistribution_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkDistribution")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch13_GpuWorkDistribution_Solution
    EXTRA_DEPENDENCIES ch13_work_distribution_solution_logic)

