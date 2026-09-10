set(
    LGP_CH33_DXR_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/DxrContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/DxrContracts.hpp")
set(
    LGP_CH33_DXR_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/DxrShared.hlsli")
set(
    LGP_CH33_DXR_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DxrLab.hlsl")
set(
    LGP_CH33_DXR_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DxrLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/DxrShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/DxrLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/DxrLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch33_dxr_contracts STATIC ${LGP_CH33_DXR_CONTRACT_SOURCES})
add_library(LGP::Ch33DxrContracts ALIAS ch33_dxr_contracts)
target_link_libraries(
    ch33_dxr_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch33_dxr_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch33_dxr_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/DxrFoundations")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH33_DXR_CONTRACT_SOURCES})

add_library(ch33_dxr_gpu_common STATIC ${LGP_CH33_DXR_GPU_COMMON_SOURCES})
target_link_libraries(
    ch33_dxr_gpu_common
    PUBLIC
        LGP::Ch33DxrContracts
        LGP::Framework)
target_include_directories(ch33_dxr_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch33_dxr_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/DxrFoundations")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH33_DXR_GPU_COMMON_SOURCES})

add_library(ch33_dxr_starter_logic STATIC ${LGP_CH33_DXR_STARTER_SOURCES})
target_link_libraries(ch33_dxr_starter_logic PUBLIC ch33_dxr_gpu_common)
target_include_directories(ch33_dxr_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch33_dxr_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/DxrFoundations")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH33_DXR_STARTER_SOURCES})

add_library(ch33_dxr_solution_logic STATIC ${LGP_CH33_DXR_SOLUTION_SOURCES})
target_link_libraries(ch33_dxr_solution_logic PUBLIC ch33_dxr_gpu_common)
target_include_directories(ch33_dxr_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch33_dxr_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/DxrFoundations")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH33_DXR_SOLUTION_SOURCES})

add_executable(Ch33_DxrFoundations_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch33_DxrFoundations_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/DxrFoundations")
lgp_configure_framework_chapter_app(
    TARGET Ch33_DxrFoundations_Starter
    EXTRA_DEPENDENCIES ch33_dxr_starter_logic)

add_executable(Ch33_DxrFoundations_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch33_DxrFoundations_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/DxrFoundations")
lgp_configure_framework_chapter_app(
    TARGET Ch33_DxrFoundations_Solution
    EXTRA_DEPENDENCIES ch33_dxr_solution_logic)

