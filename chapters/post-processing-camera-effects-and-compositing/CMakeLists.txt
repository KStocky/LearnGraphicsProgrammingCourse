set(
    LGP_CH30_POST_PROCESSING_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PostProcessingContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PostProcessingContracts.hpp")
set(
    LGP_CH30_POST_PROCESSING_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PostProcessingShared.hlsli")
set(
    LGP_CH30_POST_PROCESSING_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/PostProcessingLab.hlsl")
set(
    LGP_CH30_POST_PROCESSING_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/PostProcessingLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PostProcessingShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/PostProcessingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/PostProcessingLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch30_post_processing_contracts STATIC ${LGP_CH30_POST_PROCESSING_CONTRACT_SOURCES})
add_library(LGP::Ch30PostProcessingContracts ALIAS ch30_post_processing_contracts)
target_link_libraries(
    ch30_post_processing_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch30_post_processing_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch30_post_processing_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PostProcessing")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH30_POST_PROCESSING_CONTRACT_SOURCES})

add_library(ch30_post_processing_gpu_common STATIC ${LGP_CH30_POST_PROCESSING_GPU_COMMON_SOURCES})
target_link_libraries(
    ch30_post_processing_gpu_common
    PUBLIC
        LGP::Ch30PostProcessingContracts
        LGP::Framework)
target_include_directories(ch30_post_processing_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch30_post_processing_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PostProcessing")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH30_POST_PROCESSING_GPU_COMMON_SOURCES})

add_library(ch30_post_processing_starter_logic STATIC ${LGP_CH30_POST_PROCESSING_STARTER_SOURCES})
target_link_libraries(ch30_post_processing_starter_logic PUBLIC ch30_post_processing_gpu_common)
target_include_directories(ch30_post_processing_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch30_post_processing_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PostProcessing")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH30_POST_PROCESSING_STARTER_SOURCES})

add_library(ch30_post_processing_solution_logic STATIC ${LGP_CH30_POST_PROCESSING_SOLUTION_SOURCES})
target_link_libraries(ch30_post_processing_solution_logic PUBLIC ch30_post_processing_gpu_common)
target_include_directories(ch30_post_processing_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch30_post_processing_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PostProcessing")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH30_POST_PROCESSING_SOLUTION_SOURCES})

add_executable(Ch30_PostProcessing_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch30_PostProcessing_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PostProcessing")
lgp_configure_framework_chapter_app(
    TARGET Ch30_PostProcessing_Starter
    EXTRA_DEPENDENCIES ch30_post_processing_starter_logic)

add_executable(Ch30_PostProcessing_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch30_PostProcessing_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PostProcessing")
lgp_configure_framework_chapter_app(
    TARGET Ch30_PostProcessing_Solution
    EXTRA_DEPENDENCIES ch30_post_processing_solution_logic)

