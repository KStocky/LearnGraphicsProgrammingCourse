set(
    LGP_CH14_CLUSTERED_LIGHTING_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ClusteredLightingContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ClusteredLightingContracts.hpp")
set(
    LGP_CH14_CLUSTERED_LIGHTING_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH14_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ClusteredLightingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH14_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ClusteredLightingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

add_library(ch14_clustered_lighting_contracts STATIC ${LGP_CH14_CLUSTERED_LIGHTING_CONTRACT_SOURCES})
add_library(LGP::Ch14ClusteredLightingContracts ALIAS ch14_clustered_lighting_contracts)
target_link_libraries(
    ch14_clustered_lighting_contracts
    PUBLIC
        LGP::Ch12GBufferContracts
        LGP::CompilerFlags)
target_include_directories(
    ch14_clustered_lighting_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch14_clustered_lighting_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ClusteredLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH14_CLUSTERED_LIGHTING_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ClusteredLightingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ClusteredLightingLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch14_clustered_lighting_gpu_common STATIC ${LGP_CH14_CLUSTERED_LIGHTING_GPU_COMMON_SOURCES})
target_link_libraries(
    ch14_clustered_lighting_gpu_common
    PUBLIC
        LGP::Ch14ClusteredLightingContracts
        LGP::Framework)
target_include_directories(ch14_clustered_lighting_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch14_clustered_lighting_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ClusteredLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH14_CLUSTERED_LIGHTING_GPU_COMMON_SOURCES})

add_library(ch14_clustered_lighting_starter_logic STATIC ${LGP_CH14_STARTER_SOURCES})
target_link_libraries(
    ch14_clustered_lighting_starter_logic
    PUBLIC
        ch14_clustered_lighting_gpu_common
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch14_clustered_lighting_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch14_clustered_lighting_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ClusteredLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH14_STARTER_SOURCES})

add_library(ch14_clustered_lighting_solution_logic STATIC ${LGP_CH14_SOLUTION_SOURCES})
target_link_libraries(
    ch14_clustered_lighting_solution_logic
    PUBLIC
        ch14_clustered_lighting_gpu_common
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch14_clustered_lighting_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch14_clustered_lighting_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ClusteredLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH14_SOLUTION_SOURCES})

add_executable(Ch14_ClusteredLighting_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch14_ClusteredLighting_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ClusteredLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch14_ClusteredLighting_Starter
    EXTRA_DEPENDENCIES ch14_clustered_lighting_starter_logic)

add_executable(Ch14_ClusteredLighting_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch14_ClusteredLighting_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ClusteredLighting")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch14_ClusteredLighting_Solution
    EXTRA_DEPENDENCIES ch14_clustered_lighting_solution_logic)

