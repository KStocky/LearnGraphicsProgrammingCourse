set(
    LGP_CH12_GBUFFER_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GBufferContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GBufferContracts.hpp")
set(
    LGP_CH12_GBUFFER_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH12_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/GBufferDeferredLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH12_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/GBufferDeferredLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

add_library(ch12_gbuffer_contracts STATIC ${LGP_CH12_GBUFFER_CONTRACT_SOURCES})
add_library(LGP::Ch12GBufferContracts ALIAS ch12_gbuffer_contracts)
target_link_libraries(ch12_gbuffer_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(
    ch12_gbuffer_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch12_gbuffer_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GBuffer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH12_GBUFFER_CONTRACT_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/GBufferDeferredLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/GBufferDeferredLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch12_gbuffer_gpu_common STATIC ${LGP_CH12_GBUFFER_GPU_COMMON_SOURCES})
target_link_libraries(
    ch12_gbuffer_gpu_common
    PUBLIC
        LGP::Ch06SurfaceFrameContracts
        LGP::Ch12GBufferContracts
        LGP::Framework)
target_include_directories(ch12_gbuffer_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch12_gbuffer_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GBuffer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH12_GBUFFER_GPU_COMMON_SOURCES})

add_library(ch12_gbuffer_starter_logic STATIC ${LGP_CH12_STARTER_SOURCES})
target_link_libraries(
    ch12_gbuffer_starter_logic
    PUBLIC
        ch12_gbuffer_gpu_common
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(ch12_gbuffer_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch12_gbuffer_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GBuffer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH12_STARTER_SOURCES})

get_target_property(LGP_CH12_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH12_IMGUI_INCLUDE_DIRS 0 LGP_CH12_IMGUI_ROOT)
set(LGP_CH12_IMGUI_DX12_BACKEND "${LGP_CH12_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH12_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH12_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH12_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

add_library(
    ch12_gbuffer_solution_logic
    STATIC
        ${LGP_CH12_SOLUTION_SOURCES}
        "${LGP_CH12_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch12_gbuffer_solution_logic
    PUBLIC
        DearImGui::DearImGui
        ch12_gbuffer_gpu_common
        LGP::Framework
        Microsoft::DirectXMath)
target_include_directories(
    ch12_gbuffer_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH12_IMGUI_ROOT}"
        "${LGP_CH12_IMGUI_ROOT}/backends")
target_compile_definitions(ch12_gbuffer_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch12_gbuffer_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GBuffer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH12_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH12_IMGUI_DX12_BACKEND}")

add_executable(Ch12_GBufferDeferred_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch12_GBufferDeferred_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GBuffer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch12_GBufferDeferred_Starter
    EXTRA_DEPENDENCIES ch12_gbuffer_starter_logic)

add_executable(Ch12_GBufferDeferred_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch12_GBufferDeferred_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/GBuffer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch12_GBufferDeferred_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch12_gbuffer_solution_logic)

