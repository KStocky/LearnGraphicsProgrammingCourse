set(
    LGP_CH17_IMPORTANCE_SAMPLING_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ImportanceSamplingContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ImportanceSamplingContracts.hpp")

add_library(ch17_importance_sampling_contracts STATIC ${LGP_CH17_IMPORTANCE_SAMPLING_CONTRACT_SOURCES})
add_library(LGP::Ch17ImportanceSamplingContracts ALIAS ch17_importance_sampling_contracts)
target_link_libraries(
    ch17_importance_sampling_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch17_importance_sampling_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch17_importance_sampling_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ImportanceSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH17_IMPORTANCE_SAMPLING_CONTRACT_SOURCES})

set(
    LGP_CH17_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH17_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ImportanceSamplingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH17_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ImportanceSamplingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ImportanceSamplingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ImportanceSamplingLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch17_importance_sampling_gpu_common STATIC ${LGP_CH17_GPU_COMMON_SOURCES})
target_link_libraries(
    ch17_importance_sampling_gpu_common
    PUBLIC
        LGP::Ch17ImportanceSamplingContracts
        LGP::Framework)
target_include_directories(ch17_importance_sampling_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch17_importance_sampling_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ImportanceSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH17_GPU_COMMON_SOURCES})

add_library(ch17_importance_sampling_starter_logic STATIC ${LGP_CH17_STARTER_SOURCES})
target_link_libraries(
    ch17_importance_sampling_starter_logic
    PUBLIC
        ch17_importance_sampling_gpu_common)
target_include_directories(ch17_importance_sampling_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch17_importance_sampling_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ImportanceSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH17_STARTER_SOURCES})

add_library(ch17_importance_sampling_solution_logic STATIC ${LGP_CH17_SOLUTION_SOURCES})
target_link_libraries(
    ch17_importance_sampling_solution_logic
    PUBLIC
        ch17_importance_sampling_gpu_common)
target_include_directories(ch17_importance_sampling_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch17_importance_sampling_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ImportanceSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH17_SOLUTION_SOURCES})

add_executable(Ch17_ImportanceSampling_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch17_ImportanceSampling_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ImportanceSampling")
lgp_configure_framework_chapter_app(
    TARGET Ch17_ImportanceSampling_Starter
    EXTRA_DEPENDENCIES ch17_importance_sampling_starter_logic)

add_executable(Ch17_ImportanceSampling_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch17_ImportanceSampling_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ImportanceSampling")
lgp_configure_framework_chapter_app(
    TARGET Ch17_ImportanceSampling_Solution
    EXTRA_DEPENDENCIES ch17_importance_sampling_solution_logic)

