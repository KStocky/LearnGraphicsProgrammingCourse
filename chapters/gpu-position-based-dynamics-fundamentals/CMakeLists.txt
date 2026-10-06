add_library(
    ch40_pbd_contracts STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PbdContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PbdContracts.hpp")
target_link_libraries(ch40_pbd_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch40_pbd_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch40_pbd_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PositionBasedDynamics")


add_library(
    ch40_pbd_gpu_common STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
target_link_libraries(ch40_pbd_gpu_common PUBLIC ch40_pbd_contracts LGP::Framework)
target_include_directories(ch40_pbd_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch40_pbd_gpu_common PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/PbdLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/PbdLab.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(
    ch40_pbd_starter_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/PbdLab.hlsl")
target_compile_definitions(ch40_pbd_starter_logic PRIVATE LGP_PBD_VARIANT=starter)
target_link_libraries(ch40_pbd_starter_logic PUBLIC ch40_pbd_gpu_common)
target_include_directories(ch40_pbd_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch40_pbd_starter_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

add_library(
    ch40_pbd_solution_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/PbdLab.hlsl")
target_compile_definitions(ch40_pbd_solution_logic PRIVATE LGP_PBD_VARIANT=solution)
target_link_libraries(ch40_pbd_solution_logic PUBLIC ch40_pbd_gpu_common)
target_include_directories(ch40_pbd_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch40_pbd_solution_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

add_executable(Ch40_GpuPositionBasedDynamics_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
target_compile_definitions(Ch40_GpuPositionBasedDynamics_Starter PRIVATE LGP_PBD_VARIANT=starter)
set_target_properties(Ch40_GpuPositionBasedDynamics_Starter PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
lgp_configure_framework_chapter_app(
    TARGET Ch40_GpuPositionBasedDynamics_Starter
    EXTRA_DEPENDENCIES ch40_pbd_starter_logic)

add_executable(Ch40_GpuPositionBasedDynamics_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_compile_definitions(Ch40_GpuPositionBasedDynamics_Solution PRIVATE LGP_PBD_VARIANT=solution)
set_target_properties(Ch40_GpuPositionBasedDynamics_Solution PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
lgp_configure_framework_chapter_app(
    TARGET Ch40_GpuPositionBasedDynamics_Solution
    EXTRA_DEPENDENCIES ch40_pbd_solution_logic)

