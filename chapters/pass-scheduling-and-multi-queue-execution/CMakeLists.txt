set(
    LGP_CH10_PASS_SCHEDULING_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PassSchedule.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/PassSchedule.hpp")

add_library(ch10_pass_scheduling_contracts STATIC ${LGP_CH10_PASS_SCHEDULING_SOURCES})
add_library(LGP::Ch10PassSchedulingContracts ALIAS ch10_pass_scheduling_contracts)
target_link_libraries(
    ch10_pass_scheduling_contracts
    PUBLIC
        LGP::Ch08FrameGraphContracts
        LGP::CompilerFlags)
target_include_directories(
    ch10_pass_scheduling_contracts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch10_pass_scheduling_contracts
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PassScheduling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH10_PASS_SCHEDULING_SOURCES})

set(
    LGP_CH10_GPU_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/GpuLabSupport.hpp")
set(
    LGP_CH10_STARTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MultiQueueSchedulingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
set(
    LGP_CH10_SOLUTION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MultiQueueSchedulingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/MultiQueueSchedulingLab.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/MultiQueueSchedulingLab.hlsl"
    PROPERTIES
        HEADER_FILE_ONLY TRUE)

get_target_property(LGP_CH10_IMGUI_INCLUDE_DIRS DearImGui::DearImGui INTERFACE_INCLUDE_DIRECTORIES)
list(GET LGP_CH10_IMGUI_INCLUDE_DIRS 0 LGP_CH10_IMGUI_ROOT)
set(LGP_CH10_IMGUI_DX12_BACKEND "${LGP_CH10_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp")
set_source_files_properties("${LGP_CH10_IMGUI_DX12_BACKEND}" PROPERTIES SKIP_LINTING TRUE)
if(CMAKE_CXX_COMPILER MATCHES "clang-cl")
    set_source_files_properties(
        "${LGP_CH10_IMGUI_DX12_BACKEND}"
        PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-variable")
elseif(MSVC)
    set_source_files_properties("${LGP_CH10_IMGUI_DX12_BACKEND}" PROPERTIES COMPILE_FLAGS "/wd4189")
endif()

add_library(ch10_multi_queue_gpu_common STATIC ${LGP_CH10_GPU_COMMON_SOURCES})
target_link_libraries(
    ch10_multi_queue_gpu_common
    PUBLIC
        LGP::Ch10PassSchedulingContracts
        LGP::Framework)
target_include_directories(ch10_multi_queue_gpu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set_target_properties(
    ch10_multi_queue_gpu_common
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PassScheduling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH10_GPU_COMMON_SOURCES})

add_library(ch10_multi_queue_starter_logic STATIC ${LGP_CH10_STARTER_SOURCES})
target_link_libraries(
    ch10_multi_queue_starter_logic
    PUBLIC
        ch10_multi_queue_gpu_common
        LGP::Framework)
target_include_directories(ch10_multi_queue_starter_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ch10_multi_queue_starter_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PassScheduling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH10_STARTER_SOURCES})

add_library(
    ch10_multi_queue_solution_logic
    STATIC
        ${LGP_CH10_SOLUTION_SOURCES}
        "${LGP_CH10_IMGUI_DX12_BACKEND}")
target_link_libraries(
    ch10_multi_queue_solution_logic
    PUBLIC
        DearImGui::DearImGui
        ch10_multi_queue_gpu_common
        LGP::Framework)
target_include_directories(
    ch10_multi_queue_solution_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LGP_CH10_IMGUI_ROOT}"
        "${LGP_CH10_IMGUI_ROOT}/backends")
target_compile_definitions(ch10_multi_queue_solution_logic PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(
    ch10_multi_queue_solution_logic
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PassScheduling")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LGP_CH10_SOLUTION_SOURCES})
source_group("ThirdParty\\ImGuiBackends" FILES "${LGP_CH10_IMGUI_DX12_BACKEND}")

add_executable(Ch10_MultiQueueScheduling_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
set_target_properties(
    Ch10_MultiQueueScheduling_Starter
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PassScheduling")
lgp_configure_framework_chapter_app(
    TARGET Ch10_MultiQueueScheduling_Starter
    EXTRA_DEPENDENCIES ch10_multi_queue_starter_logic)

add_executable(Ch10_MultiQueueScheduling_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
set_target_properties(
    Ch10_MultiQueueScheduling_Solution
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        FOLDER "Chapters/PassScheduling")
lgp_configure_framework_chapter_app(
    TARGET Ch10_MultiQueueScheduling_Solution
    ENABLE_IMGUI
    EXTRA_DEPENDENCIES ch10_multi_queue_solution_logic)

