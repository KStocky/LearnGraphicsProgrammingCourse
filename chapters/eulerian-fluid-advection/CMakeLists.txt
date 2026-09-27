add_library(ch36_eulerian_contracts STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/GridContracts.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/GridContracts.hpp")
add_library(LGP::Ch36EulerianContracts ALIAS ch36_eulerian_contracts)
target_link_libraries(ch36_eulerian_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch36_eulerian_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch36_eulerian_contracts PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/EulerianFluidAdvection")

add_library(ch36_eulerian_gpu_common STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
target_link_libraries(ch36_eulerian_gpu_common PUBLIC LGP::Ch36EulerianContracts LGP::Framework)
target_include_directories(ch36_eulerian_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch36_eulerian_gpu_common PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/EulerianFluidAdvection")

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Starter/FluidLab.hlsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/FluidLab.hlsl"
                            PROPERTIES HEADER_FILE_ONLY TRUE)
add_library(ch36_eulerian_starter_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/FluidLab.hlsl")
target_link_libraries(ch36_eulerian_starter_logic PUBLIC ch36_eulerian_gpu_common)
add_library(ch36_eulerian_solution_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/FluidLab.hlsl")
target_link_libraries(ch36_eulerian_solution_logic PUBLIC ch36_eulerian_gpu_common)
set_target_properties(
    ch36_eulerian_starter_logic ch36_eulerian_solution_logic
    PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
               FOLDER "Chapters/EulerianFluidAdvection")

add_executable(Ch36_EulerianFluidAdvection_Starter
               "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
target_link_libraries(Ch36_EulerianFluidAdvection_Starter PRIVATE ch36_eulerian_starter_logic)
add_executable(Ch36_EulerianFluidAdvection_Solution
               "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_link_libraries(Ch36_EulerianFluidAdvection_Solution PRIVATE ch36_eulerian_solution_logic)
set_target_properties(
    Ch36_EulerianFluidAdvection_Starter Ch36_EulerianFluidAdvection_Solution
    PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
               FOLDER "Chapters/EulerianFluidAdvection")
lgp_configure_framework_chapter_app(TARGET Ch36_EulerianFluidAdvection_Starter)
lgp_configure_framework_chapter_app(TARGET Ch36_EulerianFluidAdvection_Solution)

