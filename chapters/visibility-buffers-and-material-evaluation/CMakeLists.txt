set(
    LGP_CH15_VISIBILITY_BUFFER_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/VisibilityBufferContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/VisibilityBufferContracts.hpp")
set(
    LGP_CH15_VISIBILITY_BUFFER_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/VisibilityBufferShared.hlsli")
set(
    LGP_CH15_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/VisibilityBufferLab.hlsl")
set(
    LGP_CH15_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/VisibilityBufferLab.hlsl")

add_library(ch15_visibility_buffer_contracts STATIC ${LGP_CH15_VISIBILITY_BUFFER_CONTRACT_SOURCES})
add_library(LGP::Ch15VisibilityBufferContracts ALIAS ch15_visibility_buffer_contracts)
target_link_libraries(
    ch15_visibility_buffer_contracts
    PUBLIC
        LGP::Ch06SurfaceFrameContracts
        LGP::Ch12GBufferContracts
        LGP::CompilerFlags)
target_include_directories(
    ch15_visibility_buffer_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch15_visibility_buffer_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/VisibilityBuffers")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH15_VISIBILITY_BUFFER_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/VisibilityBufferShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/VisibilityBufferLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/VisibilityBufferLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch15_visibility_buffer_gpu_common STATIC ${LGP_CH15_VISIBILITY_BUFFER_GPU_COMMON_SOURCES})
target_link_libraries(
    ch15_visibility_buffer_gpu_common
    PUBLIC
        LGP::Ch14ClusteredLightingContracts
        LGP::Ch15VisibilityBufferContracts
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch15_visibility_buffer_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch15_visibility_buffer_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/VisibilityBuffers")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH15_VISIBILITY_BUFFER_GPU_COMMON_SOURCES})

add_library(ch15_visibility_buffer_starter_logic STATIC ${LGP_CH15_STARTER_SOURCES})
target_link_libraries(
    ch15_visibility_buffer_starter_logic
    PUBLIC
        ch15_visibility_buffer_gpu_common
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch15_visibility_buffer_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch15_visibility_buffer_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/VisibilityBuffers")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH15_STARTER_SOURCES})

add_library(ch15_visibility_buffer_solution_logic STATIC ${LGP_CH15_SOLUTION_SOURCES})
target_link_libraries(
    ch15_visibility_buffer_solution_logic
    PUBLIC
        ch15_visibility_buffer_gpu_common
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch15_visibility_buffer_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch15_visibility_buffer_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/VisibilityBuffers")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH15_SOLUTION_SOURCES})

add_executable(Ch15_VisibilityBuffer_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch15_VisibilityBuffer_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/VisibilityBuffers")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch15_VisibilityBuffer_Starter
    EXTRA_DEPENDENCIES ch15_visibility_buffer_starter_logic)

add_executable(Ch15_VisibilityBuffer_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch15_VisibilityBuffer_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/VisibilityBuffers")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch15_VisibilityBuffer_Solution
    EXTRA_DEPENDENCIES ch15_visibility_buffer_solution_logic)

