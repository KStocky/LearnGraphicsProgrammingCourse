set(
    LGP_CH25_BLUE_NOISE_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/BlueNoiseContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/BlueNoiseContracts.hpp")

add_library(ch25_blue_noise_contracts STATIC ${LGP_CH25_BLUE_NOISE_CONTRACT_SOURCES})
add_library(LGP::Ch25BlueNoiseContracts ALIAS ch25_blue_noise_contracts)
target_link_libraries(
    ch25_blue_noise_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(ch25_blue_noise_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch25_blue_noise_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/BlueNoiseStochasticReconstruction")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH25_BLUE_NOISE_CONTRACT_SOURCES})

set(
    LGP_CH25_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH25_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/BlueNoiseLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH25_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/BlueNoiseLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/BlueNoiseLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/BlueNoiseLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch25_blue_noise_gpu_common STATIC ${LGP_CH25_GPU_COMMON_SOURCES})
target_link_libraries(
    ch25_blue_noise_gpu_common
    PUBLIC
        LGP::Ch25BlueNoiseContracts
        LGP::Framework)
target_include_directories(ch25_blue_noise_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch25_blue_noise_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/BlueNoiseStochasticReconstruction")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH25_GPU_COMMON_SOURCES})

add_library(ch25_blue_noise_starter_logic STATIC ${LGP_CH25_STARTER_SOURCES})
target_link_libraries(
    ch25_blue_noise_starter_logic
    PUBLIC
        ch25_blue_noise_gpu_common)
target_include_directories(ch25_blue_noise_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch25_blue_noise_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/BlueNoiseStochasticReconstruction")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH25_STARTER_SOURCES})

add_library(ch25_blue_noise_solution_logic STATIC ${LGP_CH25_SOLUTION_SOURCES})
target_link_libraries(
    ch25_blue_noise_solution_logic
    PUBLIC
        ch25_blue_noise_gpu_common)
target_include_directories(ch25_blue_noise_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch25_blue_noise_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/BlueNoiseStochasticReconstruction")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH25_SOLUTION_SOURCES})

add_executable(Ch25_BlueNoise_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch25_BlueNoise_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/BlueNoiseStochasticReconstruction")
lgp_configure_framework_chapter_app(
    TARGET Ch25_BlueNoise_Starter
    EXTRA_DEPENDENCIES ch25_blue_noise_starter_logic)

add_executable(Ch25_BlueNoise_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch25_BlueNoise_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/BlueNoiseStochasticReconstruction")
lgp_configure_framework_chapter_app(
    TARGET Ch25_BlueNoise_Solution
    EXTRA_DEPENDENCIES ch25_blue_noise_solution_logic)

