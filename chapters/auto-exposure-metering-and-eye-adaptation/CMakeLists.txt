set(
    LGP_CH31_AUTO_EXPOSURE_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/AutoExposureContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/AutoExposureContracts.hpp")
set(
    LGP_CH31_AUTO_EXPOSURE_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/AutoExposureShared.hlsli")
set(
    LGP_CH31_AUTO_EXPOSURE_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/AutoExposureLab.hlsl")
set(
    LGP_CH31_AUTO_EXPOSURE_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/AutoExposureLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/AutoExposureShared.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/AutoExposureLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/AutoExposureLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch31_auto_exposure_contracts STATIC ${LGP_CH31_AUTO_EXPOSURE_CONTRACT_SOURCES})
add_library(LGP::Ch31AutoExposureContracts ALIAS ch31_auto_exposure_contracts)
target_link_libraries(
    ch31_auto_exposure_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch31_auto_exposure_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch31_auto_exposure_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/AutoExposure")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH31_AUTO_EXPOSURE_CONTRACT_SOURCES})

add_library(ch31_auto_exposure_gpu_common STATIC ${LGP_CH31_AUTO_EXPOSURE_GPU_COMMON_SOURCES})
target_link_libraries(
    ch31_auto_exposure_gpu_common
    PUBLIC
        LGP::Ch31AutoExposureContracts
        LGP::Framework)
target_include_directories(ch31_auto_exposure_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch31_auto_exposure_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/AutoExposure")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH31_AUTO_EXPOSURE_GPU_COMMON_SOURCES})

add_library(ch31_auto_exposure_starter_logic STATIC ${LGP_CH31_AUTO_EXPOSURE_STARTER_SOURCES})
target_link_libraries(ch31_auto_exposure_starter_logic PUBLIC ch31_auto_exposure_gpu_common)
target_include_directories(ch31_auto_exposure_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch31_auto_exposure_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/AutoExposure")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH31_AUTO_EXPOSURE_STARTER_SOURCES})

add_library(ch31_auto_exposure_solution_logic STATIC ${LGP_CH31_AUTO_EXPOSURE_SOLUTION_SOURCES})
target_link_libraries(ch31_auto_exposure_solution_logic PUBLIC ch31_auto_exposure_gpu_common)
target_include_directories(ch31_auto_exposure_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch31_auto_exposure_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/AutoExposure")
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${LGP_CH31_AUTO_EXPOSURE_SOLUTION_SOURCES})

add_executable(Ch31_AutoExposure_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch31_AutoExposure_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/AutoExposure")
lgp_configure_framework_chapter_app(
    TARGET Ch31_AutoExposure_Starter
    EXTRA_DEPENDENCIES ch31_auto_exposure_starter_logic)

add_executable(Ch31_AutoExposure_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch31_AutoExposure_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/AutoExposure")
lgp_configure_framework_chapter_app(
    TARGET Ch31_AutoExposure_Solution
    EXTRA_DEPENDENCIES ch31_auto_exposure_solution_logic)

