set(
    LGP_CH20_WORK_GRAPHS_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/WorkGraphsContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/WorkGraphsContracts.hpp")
set(
    LGP_CH20_WORK_GRAPHS_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH20_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/WorkGraphsLab.hlsl")
set(
    LGP_CH20_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/WorkGraphsLab.hlsl")

add_library(ch20_work_graphs_contracts STATIC ${LGP_CH20_WORK_GRAPHS_CONTRACT_SOURCES})
add_library(LGP::Ch20WorkGraphsContracts ALIAS ch20_work_graphs_contracts)
target_link_libraries(ch20_work_graphs_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(
    ch20_work_graphs_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch20_work_graphs_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkGraphs")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH20_WORK_GRAPHS_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/WorkGraphsLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/WorkGraphsLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch20_work_graphs_gpu_common STATIC ${LGP_CH20_WORK_GRAPHS_GPU_COMMON_SOURCES})
target_link_libraries(
    ch20_work_graphs_gpu_common
    PUBLIC
        LGP::Ch20WorkGraphsContracts
        LGP::Framework)
target_include_directories(ch20_work_graphs_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch20_work_graphs_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkGraphs")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH20_WORK_GRAPHS_GPU_COMMON_SOURCES})

add_library(ch20_work_graphs_starter_logic STATIC ${LGP_CH20_STARTER_SOURCES})
target_link_libraries(ch20_work_graphs_starter_logic PUBLIC ch20_work_graphs_gpu_common)
target_include_directories(ch20_work_graphs_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch20_work_graphs_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkGraphs")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH20_STARTER_SOURCES})

add_library(ch20_work_graphs_solution_logic STATIC ${LGP_CH20_SOLUTION_SOURCES})
target_link_libraries(ch20_work_graphs_solution_logic PUBLIC ch20_work_graphs_gpu_common)
target_include_directories(ch20_work_graphs_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch20_work_graphs_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkGraphs")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH20_SOLUTION_SOURCES})

add_executable(Ch20_WorkGraphs_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch20_WorkGraphs_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkGraphs")
lgp_configure_framework_chapter_app(
    TARGET Ch20_WorkGraphs_Starter
    EXTRA_DEPENDENCIES ch20_work_graphs_starter_logic)

add_executable(Ch20_WorkGraphs_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch20_WorkGraphs_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/WorkGraphs")
lgp_configure_framework_chapter_app(
    TARGET Ch20_WorkGraphs_Solution
    EXTRA_DEPENDENCIES ch20_work_graphs_solution_logic)

