set(
    LGP_CH09_TRANSIENT_ALLOCATION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransientTextureAllocation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransientTextureAllocation.hpp")

add_library(ch09_transient_allocation_contracts STATIC ${LGP_CH09_TRANSIENT_ALLOCATION_SOURCES})
add_library(LGP::Ch09TransientAllocationContracts ALIAS ch09_transient_allocation_contracts)
target_link_libraries(
    ch09_transient_allocation_contracts
    PUBLIC
        LGP::Ch08FrameGraphContracts
        LGP::CompilerFlags)
target_include_directories(
    ch09_transient_allocation_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch09_transient_allocation_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TransientAllocation")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH09_TRANSIENT_ALLOCATION_SOURCES})

set(
    LGP_CH09_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH09_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TransientAliasingLab.hlsl")
set(
    LGP_CH09_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TransientAliasingLab.hlsl")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/TransientAliasingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/TransientAliasingLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

get_target_property(LGP_CH09_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH09_IMGUI_INCLUDE_DIRS 0 LGP_CH09_IMGUI_ROOT)
set(LGP_CH09_IMGUI_DX12_BACKEND "${LGP_CH09_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH09_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH09_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH09_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

add_library(ch09_transient_aliasing_gpu_common STATIC ${LGP_CH09_GPU_COMMON_SOURCES})
target_link_libraries(
    ch09_transient_aliasing_gpu_common
    PUBLIC
        LGP::Framework)
target_include_directories(ch09_transient_aliasing_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch09_transient_aliasing_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TransientAllocation")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH09_GPU_COMMON_SOURCES})

add_library(ch09_transient_aliasing_starter_logic STATIC ${LGP_CH09_STARTER_SOURCES})
target_link_libraries(
    ch09_transient_aliasing_starter_logic
    PUBLIC
        ch09_transient_aliasing_gpu_common
        LGP::Framework)
target_include_directories(ch09_transient_aliasing_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch09_transient_aliasing_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TransientAllocation")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH09_STARTER_SOURCES})

add_library(
    ch09_transient_aliasing_solution_logic
    STATIC
        ${LGP_CH09_SOLUTION_SOURCES}
        "${LGP_CH09_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch09_transient_aliasing_solution_logic
    PUBLIC
        DearImGui::DearImGui
        ch09_transient_aliasing_gpu_common
        LGP::Ch09TransientAllocationContracts
        LGP::Framework)
target_include_directories(
    ch09_transient_aliasing_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH09_IMGUI_ROOT}"
        "${LGP_CH09_IMGUI_ROOT}/backends")
target_compile_definitions(ch09_transient_aliasing_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch09_transient_aliasing_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TransientAllocation")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH09_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH09_IMGUI_DX12_BACKEND}")

add_executable(Ch09_TransientAliasing_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch09_TransientAliasing_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TransientAllocation")
lgp_configure_framework_chapter_app(
    TARGET Ch09_TransientAliasing_Starter
    EXTRA_DEPENDENCIES ch09_transient_aliasing_starter_logic)

add_executable(Ch09_TransientAliasing_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch09_TransientAliasing_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/TransientAllocation")
lgp_configure_framework_chapter_app(
    TARGET Ch09_TransientAliasing_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch09_transient_aliasing_solution_logic)

