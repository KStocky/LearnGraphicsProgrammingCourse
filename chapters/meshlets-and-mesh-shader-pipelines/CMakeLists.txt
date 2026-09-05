set(
    LGP_CH22_MESHLETS_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MeshletContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MeshletContracts.hpp")
set(
    LGP_CH22_MESHLETS_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MeshletLab.hlsli")
set(
    LGP_CH22_MESHLETS_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MeshletLab.hlsl")
set(
    LGP_CH22_MESHLETS_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MeshletLab.hlsl")

add_library(ch22_meshlets_contracts STATIC ${LGP_CH22_MESHLETS_CONTRACT_SOURCES})
add_library(LGP::Ch22MeshletsContracts ALIAS ch22_meshlets_contracts)
target_link_libraries(ch22_meshlets_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch22_meshlets_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch22_meshlets_contracts PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                                                         FOLDER "Chapters/Meshlets")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH22_MESHLETS_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MeshletLab.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MeshletLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MeshletLab.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(ch22_meshlets_common STATIC ${LGP_CH22_MESHLETS_GPU_COMMON_SOURCES})
target_link_libraries(ch22_meshlets_common PUBLIC LGP::Ch22MeshletsContracts LGP::Framework)
target_include_directories(ch22_meshlets_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch22_meshlets_common PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                                                      FOLDER "Chapters/Meshlets")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH22_MESHLETS_GPU_COMMON_SOURCES})

add_library(ch22_meshlets_starter_logic STATIC ${LGP_CH22_MESHLETS_STARTER_SOURCES})
target_link_libraries(ch22_meshlets_starter_logic PUBLIC ch22_meshlets_common)
target_include_directories(ch22_meshlets_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch22_meshlets_starter_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
                                                             CXX_EXTENSIONS NO FOLDER "Chapters/Meshlets")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH22_MESHLETS_STARTER_SOURCES})

add_library(ch22_meshlets_solution_logic STATIC ${LGP_CH22_MESHLETS_SOLUTION_SOURCES})
target_link_libraries(ch22_meshlets_solution_logic PUBLIC ch22_meshlets_common)
target_include_directories(ch22_meshlets_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch22_meshlets_solution_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
                                                              CXX_EXTENSIONS NO FOLDER "Chapters/Meshlets")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH22_MESHLETS_SOLUTION_SOURCES})

add_executable(Ch22_Meshlets_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(Ch22_Meshlets_Starter PROPERTIES FOLDER "Chapters/Meshlets")
lgp_configure_framework_chapter_app(
    TARGET Ch22_Meshlets_Starter
    EXTRA_DEPENDENCIES ch22_meshlets_starter_logic)

add_executable(Ch22_Meshlets_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(Ch22_Meshlets_Solution PROPERTIES FOLDER "Chapters/Meshlets")
lgp_configure_framework_chapter_app(
    TARGET Ch22_Meshlets_Solution
    EXTRA_DEPENDENCIES ch22_meshlets_solution_logic)

