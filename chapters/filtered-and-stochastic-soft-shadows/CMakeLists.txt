add_library(ch38_soft_shadow_contracts STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/SoftShadowContracts.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/SoftShadowContracts.hpp")
add_library(LGP::Ch38SoftShadowContracts ALIAS ch38_soft_shadow_contracts)
target_link_libraries(ch38_soft_shadow_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch38_soft_shadow_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch38_soft_shadow_contracts PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/SoftShadows")

add_library(ch38_gpu_support STATIC "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuSupport.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuSupport.hpp")
target_link_libraries(ch38_gpu_support PUBLIC ch38_soft_shadow_contracts LGP::Framework)
target_include_directories(ch38_gpu_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch38_gpu_support PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/SoftShadows")

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Starter/SoftShadowLab.hlsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SoftShadowLab.hlsl"
                            PROPERTIES HEADER_FILE_ONLY TRUE)
add_library(ch38_starter_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Starter/SoftShadowLab.hlsl")
add_library(ch38_solution_logic STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Solution/SoftShadowLab.hlsl")
target_link_libraries(ch38_starter_logic PUBLIC ch38_gpu_support)
target_link_libraries(ch38_solution_logic PUBLIC ch38_gpu_support)
set_target_properties(ch38_starter_logic ch38_solution_logic PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/SoftShadows")

add_executable(Ch38_SoftShadows_Starter
               "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
add_executable(Ch38_SoftShadows_Solution
               "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_link_libraries(Ch38_SoftShadows_Starter PRIVATE ch38_starter_logic)
target_link_libraries(Ch38_SoftShadows_Solution PRIVATE ch38_solution_logic)
set_target_properties(Ch38_SoftShadows_Starter Ch38_SoftShadows_Solution PROPERTIES
                      CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                      FOLDER "Chapters/SoftShadows")
lgp_configure_framework_chapter_app(TARGET Ch38_SoftShadows_Starter)
lgp_configure_framework_chapter_app(TARGET Ch38_SoftShadows_Solution)

