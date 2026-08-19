set(
    LGP_CH11_REPROJECTION_CONTRACT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/Reprojection.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/Reprojection.hpp")
set(
    LGP_CH11_REPROJECTION_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH11_REPROJECTION_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ReprojectionLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH11_REPROJECTION_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ReprojectionLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/ReprojectionLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/ReprojectionLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

add_library(ch11_reprojection_contracts STATIC ${LGP_CH11_REPROJECTION_CONTRACT_SOURCES})
add_library(LGP::Ch11ReprojectionContracts ALIAS ch11_reprojection_contracts)
target_link_libraries(ch11_reprojection_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(
    ch11_reprojection_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch11_reprojection_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Reprojection")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH11_REPROJECTION_CONTRACT_SOURCES})

add_library(ch11_reprojection_gpu_common STATIC ${LGP_CH11_REPROJECTION_GPU_COMMON_SOURCES})
target_link_libraries(
    ch11_reprojection_gpu_common
    PUBLIC
        LGP::Ch11ReprojectionContracts
        LGP::Framework)
target_include_directories(ch11_reprojection_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch11_reprojection_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Reprojection")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH11_REPROJECTION_GPU_COMMON_SOURCES})

get_target_property(LGP_CH11_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH11_IMGUI_INCLUDE_DIRS 0 LGP_CH11_IMGUI_ROOT)
set(LGP_CH11_IMGUI_DX12_BACKEND "${LGP_CH11_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH11_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH11_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH11_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

add_library(
    ch11_reprojection_starter_logic
    STATIC
        ${LGP_CH11_REPROJECTION_STARTER_SOURCES}
        "${LGP_CH11_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch11_reprojection_starter_logic
    PUBLIC
        DearImGui::DearImGui
        ch11_reprojection_gpu_common
        LGP::Framework)
target_include_directories(
    ch11_reprojection_starter_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH11_IMGUI_ROOT}"
        "${LGP_CH11_IMGUI_ROOT}/backends")
target_compile_definitions(ch11_reprojection_starter_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch11_reprojection_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Reprojection")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH11_REPROJECTION_STARTER_SOURCES})

add_library(
    ch11_reprojection_solution_logic
    STATIC
        ${LGP_CH11_REPROJECTION_SOLUTION_SOURCES}
        "${LGP_CH11_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch11_reprojection_solution_logic
    PUBLIC
        DearImGui::DearImGui
        ch11_reprojection_gpu_common
        LGP::Framework)
target_include_directories(
    ch11_reprojection_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH11_IMGUI_ROOT}"
        "${LGP_CH11_IMGUI_ROOT}/backends")
target_compile_definitions(ch11_reprojection_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch11_reprojection_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Reprojection")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH11_REPROJECTION_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH11_IMGUI_DX12_BACKEND}")

add_executable(Ch11_Reprojection_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch11_Reprojection_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Reprojection")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch11_Reprojection_Starter
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch11_reprojection_starter_logic)

add_executable(Ch11_Reprojection_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch11_Reprojection_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/Reprojection")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
lgp_configure_framework_chapter_app(
    TARGET Ch11_Reprojection_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch11_reprojection_solution_logic)

