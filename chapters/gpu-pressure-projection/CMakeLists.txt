add_library(ch37_projection_contracts STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/ProjectionContracts.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/ProjectionContracts.hpp")
add_library(LGP::Ch37ProjectionContracts ALIAS ch37_projection_contracts)
target_link_libraries(ch37_projection_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch37_projection_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch37_projection_contracts PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/GpuPressureProjection")

add_library(ch37_gpu_support STATIC "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuSupport.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuSupport.hpp")
target_link_libraries(ch37_gpu_support PUBLIC ch37_projection_contracts LGP::Framework)
set_target_properties(ch37_gpu_support PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/GpuPressureProjection")

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Starter/ProjectionParallel.hlsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ProjectionParallel.hlsl"
                            PROPERTIES HEADER_FILE_ONLY TRUE)
add_library(ch37_starter_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ProjectionParallel.hlsl")
add_library(ch37_solution_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ProjectionParallel.hlsl")
target_link_libraries(ch37_starter_logic PUBLIC ch37_gpu_support)
target_link_libraries(ch37_solution_logic PUBLIC ch37_gpu_support)
set_target_properties(ch37_starter_logic ch37_solution_logic PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/GpuPressureProjection")

add_executable(Ch37_GpuPressureProjection_Starter
               "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
add_executable(Ch37_GpuPressureProjection_Solution
               "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_link_libraries(Ch37_GpuPressureProjection_Starter PRIVATE ch37_starter_logic)
target_link_libraries(Ch37_GpuPressureProjection_Solution PRIVATE ch37_solution_logic)
set_target_properties(Ch37_GpuPressureProjection_Starter Ch37_GpuPressureProjection_Solution PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/GpuPressureProjection")
lgp_configure_framework_chapter_app(TARGET Ch37_GpuPressureProjection_Starter)
lgp_configure_framework_chapter_app(TARGET Ch37_GpuPressureProjection_Solution)

