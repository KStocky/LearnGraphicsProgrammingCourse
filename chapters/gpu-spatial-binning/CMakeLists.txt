add_library(
    ch35_spatial_contracts STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SpatialContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SpatialContracts.hpp")
add_library(LGP::Ch35SpatialContracts ALIAS ch35_spatial_contracts)
target_link_libraries(ch35_spatial_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch35_spatial_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch35_spatial_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GpuSpatialBinning")

set(LGP_CH35_GPU_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
add_library(ch35_spatial_gpu_common STATIC ${LGP_CH35_GPU_SOURCES})
target_link_libraries(ch35_spatial_gpu_common PUBLIC LGP::Ch35SpatialContracts LGP::Framework)
target_include_directories(ch35_spatial_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch35_spatial_gpu_common PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/GpuSpatialBinning")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SpatialLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SpatialLab.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(ch35_spatial_starter_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SpatialLab.hlsl")
target_link_libraries(ch35_spatial_starter_logic PUBLIC ch35_spatial_gpu_common)
add_library(ch35_spatial_solution_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SpatialLab.hlsl")
target_link_libraries(ch35_spatial_solution_logic PUBLIC ch35_spatial_gpu_common)
set_target_properties(
    ch35_spatial_starter_logic ch35_spatial_solution_logic
    PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
               FOLDER "Chapters/GpuSpatialBinning")

add_executable(Ch35_GpuSpatialBinning_Starter
               "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
target_link_libraries(Ch35_GpuSpatialBinning_Starter PRIVATE ch35_spatial_starter_logic)
add_executable(Ch35_GpuSpatialBinning_Solution
               "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_link_libraries(Ch35_GpuSpatialBinning_Solution PRIVATE ch35_spatial_solution_logic)
set_target_properties(
    Ch35_GpuSpatialBinning_Starter Ch35_GpuSpatialBinning_Solution
    PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
               FOLDER "Chapters/GpuSpatialBinning")
lgp_configure_framework_chapter_app(TARGET Ch35_GpuSpatialBinning_Starter)
lgp_configure_framework_chapter_app(TARGET Ch35_GpuSpatialBinning_Solution)

