add_library(ch41_cloth_contracts STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ClothContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/ClothContracts.hpp")
target_link_libraries(ch41_cloth_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch41_cloth_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")

add_library(ch41_cloth_gpu_common STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
target_link_libraries(ch41_cloth_gpu_common PUBLIC ch41_cloth_contracts LGP::Framework)
target_include_directories(ch41_cloth_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ClothLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ClothLab.hlsl"
    PROPERTIES HEADER_FILE_ONLY TRUE)
add_library(ch41_cloth_starter_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ClothLab.hlsl")
target_compile_definitions(ch41_cloth_starter_logic PRIVATE LGP_CLOTH_VARIANT=starter)
target_include_directories(ch41_cloth_starter_logic BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ch41_cloth_starter_logic PUBLIC ch41_cloth_gpu_common)

add_library(ch41_cloth_solution_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ClothLab.hlsl")
target_compile_definitions(ch41_cloth_solution_logic PRIVATE LGP_CLOTH_VARIANT=solution)
target_include_directories(ch41_cloth_solution_logic BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ch41_cloth_solution_logic PUBLIC ch41_cloth_gpu_common)

add_executable(Ch41_GpuClothSoftBodies_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
target_compile_definitions(Ch41_GpuClothSoftBodies_Starter PRIVATE LGP_CLOTH_VARIANT=starter)
lgp_configure_framework_chapter_app(TARGET Ch41_GpuClothSoftBodies_Starter EXTRA_DEPENDENCIES ch41_cloth_starter_logic)
add_executable(Ch41_GpuClothSoftBodies_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_compile_definitions(Ch41_GpuClothSoftBodies_Solution PRIVATE LGP_CLOTH_VARIANT=solution)
lgp_configure_framework_chapter_app(TARGET Ch41_GpuClothSoftBodies_Solution EXTRA_DEPENDENCIES ch41_cloth_solution_logic)
set_target_properties(ch41_cloth_contracts PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
set_target_properties(ch41_cloth_gpu_common PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
set_target_properties(ch41_cloth_starter_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
set_target_properties(ch41_cloth_solution_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
set_target_properties(Ch41_GpuClothSoftBodies_Starter PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
set_target_properties(Ch41_GpuClothSoftBodies_Solution PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
