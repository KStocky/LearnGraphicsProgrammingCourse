set(
    LGP_CH16_MONTE_CARLO_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MonteCarloContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/MonteCarloContracts.hpp")

add_library(ch16_monte_carlo_contracts STATIC ${LGP_CH16_MONTE_CARLO_CONTRACT_SOURCES})
add_library(LGP::Ch16MonteCarloContracts ALIAS ch16_monte_carlo_contracts)
target_link_libraries(
    ch16_monte_carlo_contracts
    PUBLIC
        LGP::CompilerFlags)
target_include_directories(
    ch16_monte_carlo_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch16_monte_carlo_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MonteCarloSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH16_MONTE_CARLO_CONTRACT_SOURCES})

set(
    LGP_CH16_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH16_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MonteCarloLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH16_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MonteCarloLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MonteCarloLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MonteCarloLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch16_monte_carlo_gpu_common STATIC ${LGP_CH16_GPU_COMMON_SOURCES})
target_link_libraries(
    ch16_monte_carlo_gpu_common
    PUBLIC
        LGP::Ch16MonteCarloContracts
        LGP::Framework)
target_include_directories(ch16_monte_carlo_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch16_monte_carlo_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MonteCarloSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH16_GPU_COMMON_SOURCES})

add_library(ch16_monte_carlo_starter_logic STATIC ${LGP_CH16_STARTER_SOURCES})
target_link_libraries(
    ch16_monte_carlo_starter_logic
    PUBLIC
        ch16_monte_carlo_gpu_common)
target_include_directories(ch16_monte_carlo_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch16_monte_carlo_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MonteCarloSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH16_STARTER_SOURCES})

add_library(ch16_monte_carlo_solution_logic STATIC ${LGP_CH16_SOLUTION_SOURCES})
target_link_libraries(
    ch16_monte_carlo_solution_logic
    PUBLIC
        ch16_monte_carlo_gpu_common)
target_include_directories(ch16_monte_carlo_solution_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch16_monte_carlo_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MonteCarloSampling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH16_SOLUTION_SOURCES})

add_executable(Ch16_MonteCarlo_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch16_MonteCarlo_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MonteCarloSampling")
lgp_configure_framework_chapter_app(
    TARGET Ch16_MonteCarlo_Starter
    EXTRA_DEPENDENCIES ch16_monte_carlo_starter_logic)

add_executable(Ch16_MonteCarlo_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch16_MonteCarlo_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/MonteCarloSampling")
lgp_configure_framework_chapter_app(
    TARGET Ch16_MonteCarlo_Solution
    EXTRA_DEPENDENCIES ch16_monte_carlo_solution_logic)

