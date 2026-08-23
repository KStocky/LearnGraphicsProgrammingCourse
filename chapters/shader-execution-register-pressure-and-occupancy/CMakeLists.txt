set(
    LGP_CH18_SHADER_OCCUPANCY_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ShaderOccupancyContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ShaderOccupancyContracts.hpp")

add_library(ch18_shader_occupancy_contracts STATIC ${LGP_CH18_SHADER_OCCUPANCY_CONTRACT_SOURCES})
add_library(LGP::Ch18ShaderOccupancyContracts ALIAS ch18_shader_occupancy_contracts)
target_link_libraries(
    ch18_shader_occupancy_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch18_shader_occupancy_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch18_shader_occupancy_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ShaderOccupancy")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH18_SHADER_OCCUPANCY_CONTRACT_SOURCES})

set(
    LGP_CH18_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH18_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ShaderOccupancyLab.hlsl")
set(
    LGP_CH18_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ShaderOccupancyLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ShaderOccupancyLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ShaderOccupancyLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch18_shader_occupancy_gpu_common STATIC ${LGP_CH18_GPU_COMMON_SOURCES})
target_link_libraries(
    ch18_shader_occupancy_gpu_common
    PUBLIC
        LGP::Ch18ShaderOccupancyContracts
        LGP::Framework)
target_include_directories(ch18_shader_occupancy_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch18_shader_occupancy_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ShaderOccupancy")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH18_GPU_COMMON_SOURCES})

add_library(ch18_shader_occupancy_starter_logic STATIC ${LGP_CH18_STARTER_SOURCES})
target_link_libraries(ch18_shader_occupancy_starter_logic PUBLIC ch18_shader_occupancy_gpu_common)
target_include_directories(ch18_shader_occupancy_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch18_shader_occupancy_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ShaderOccupancy")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH18_STARTER_SOURCES})

add_library(ch18_shader_occupancy_solution_logic STATIC ${LGP_CH18_SOLUTION_SOURCES})
target_link_libraries(ch18_shader_occupancy_solution_logic PUBLIC ch18_shader_occupancy_gpu_common)
target_include_directories(ch18_shader_occupancy_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch18_shader_occupancy_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ShaderOccupancy")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH18_SOLUTION_SOURCES})

add_executable(Ch18_ShaderOccupancy_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch18_ShaderOccupancy_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ShaderOccupancy")
lgp_configure_framework_chapter_app(
    TARGET Ch18_ShaderOccupancy_Starter
    EXTRA_DEPENDENCIES ch18_shader_occupancy_starter_logic)

add_executable(Ch18_ShaderOccupancy_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch18_ShaderOccupancy_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/ShaderOccupancy")
lgp_configure_framework_chapter_app(
    TARGET Ch18_ShaderOccupancy_Solution
    EXTRA_DEPENDENCIES ch18_shader_occupancy_solution_logic)

