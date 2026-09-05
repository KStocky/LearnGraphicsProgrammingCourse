set(
    LGP_CH21_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuDrivenContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuDrivenContracts.hpp")
set(
    LGP_CH21_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH21_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/GpuDrivenLab.hlsl")
set(
    LGP_CH21_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/GpuDrivenLab.hlsl")

add_library(ch21_gpu_driven_contracts STATIC ${LGP_CH21_CONTRACT_SOURCES})
add_library(LGP::Ch21GpuDrivenContracts ALIAS ch21_gpu_driven_contracts)
target_link_libraries(ch21_gpu_driven_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch21_gpu_driven_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch21_gpu_driven_contracts PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                                                           FOLDER "Chapters/GpuDrivenSubmission")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/GpuDrivenLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/GpuDrivenLab.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(ch21_gpu_driven_common STATIC ${LGP_CH21_GPU_COMMON_SOURCES})
target_link_libraries(ch21_gpu_driven_common PUBLIC LGP::Ch21GpuDrivenContracts LGP::Framework)
target_include_directories(ch21_gpu_driven_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(ch21_gpu_driven_common PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
                                                        FOLDER "Chapters/GpuDrivenSubmission")

add_library(ch21_gpu_driven_starter_logic STATIC ${LGP_CH21_STARTER_SOURCES})
target_link_libraries(ch21_gpu_driven_starter_logic PUBLIC ch21_gpu_driven_common)
target_include_directories(ch21_gpu_driven_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch21_gpu_driven_starter_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
                                                               CXX_EXTENSIONS NO FOLDER "Chapters/GpuDrivenSubmission")

add_library(ch21_gpu_driven_solution_logic STATIC ${LGP_CH21_SOLUTION_SOURCES})
target_link_libraries(ch21_gpu_driven_solution_logic PUBLIC ch21_gpu_driven_common)
target_include_directories(ch21_gpu_driven_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ch21_gpu_driven_solution_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
                                                                CXX_EXTENSIONS NO FOLDER "Chapters/GpuDrivenSubmission")

add_executable(Ch21_GpuDrivenSubmission_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(Ch21_GpuDrivenSubmission_Starter PROPERTIES FOLDER "Chapters/GpuDrivenSubmission")
lgp_configure_framework_chapter_app(
    TARGET Ch21_GpuDrivenSubmission_Starter
    EXTRA_DEPENDENCIES ch21_gpu_driven_starter_logic)

add_executable(Ch21_GpuDrivenSubmission_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(Ch21_GpuDrivenSubmission_Solution PROPERTIES FOLDER "Chapters/GpuDrivenSubmission")
lgp_configure_framework_chapter_app(
    TARGET Ch21_GpuDrivenSubmission_Solution
    EXTRA_DEPENDENCIES ch21_gpu_driven_solution_logic)

