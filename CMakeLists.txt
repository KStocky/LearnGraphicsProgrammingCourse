cmake_minimum_required(VERSION 4.1)

project(
    LearnGraphicsProgramming
    VERSION 0.1.0
    DESCRIPTION "Standalone learner distribution for Learn Graphics Programming"
    HOMEPAGE_URL "https://github.com/KStocky/LearnGraphicsProgrammingCourse"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "LearnGraphicsProgramming supports Windows 11 only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LearnGraphicsProgramming supports x64 builds only.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

option(LGP_BUILD_CHAPTERS "Build chapter Starter and Solution applications" ON)
set(LGP_BUILD_TESTS OFF CACHE BOOL "Tests are not included in the public learner distribution" FORCE)
option(LGP_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(LGP_ENABLE_IMGUI "Enable the chapter diagnostics UI" ON)
option(LGP_ENABLE_D3D12_DEBUG_LAYER "Enable D3D12 debug-layer support" ON)
option(LGP_ENABLE_CLANG_TIDY "Run clang-tidy while compiling first-party targets" OFF)
set(
    LGP_SELECTED_CHAPTERS
    ""
    CACHE STRING
    "Semicolon-separated chapter directories to configure; empty selects every published chapter")
set(
    LGP_IMPLEMENTED_CHAPTERS
    graphics-math-coordinate-transforms
    rasterization-depth-and-culling
    texture-sampling-mipmapping-and-reconstruction
    color-spaces-hdr-exposure-and-tone-mapping
    radiometry-light-units-and-brdfs
    surface-frames-material-textures-and-normal-mapping
    shadow-map-fundamentals-and-bias
    frame-graph-and-resource-lifetimes
    transient-resource-allocation-and-aliasing
    pass-scheduling-and-multi-queue-execution
    gpu-work-distribution-and-indirect-execution
    shader-execution-register-pressure-and-occupancy
    d3d12-work-graphs-and-dynamic-gpu-scheduling
    gpu-driven-scene-submission-culling-and-lod
    meshlets-and-mesh-shader-pipelines
    motion-vectors-and-reprojection
    gbuffer-and-deferred-shading
    tiled-forward-plus-and-clustered-lighting
    visibility-buffers-and-material-evaluation
    monte-carlo-sampling-fundamentals
    importance-sampling-and-multiple-importance-sampling)

foreach(chapter IN LISTS LGP_SELECTED_CHAPTERS)
    if(NOT chapter IN_LIST LGP_IMPLEMENTED_CHAPTERS)
        message(FATAL_ERROR "Unknown LGP_SELECTED_CHAPTERS entry: ${chapter}")
    endif()
endforeach()

function(lgp_chapter_is_selected chapter result)
    if(NOT LGP_SELECTED_CHAPTERS OR chapter IN_LIST LGP_SELECTED_CHAPTERS)
        set("${result}" TRUE PARENT_SCOPE)
    else()
        set("${result}" FALSE PARENT_SCOPE)
    endif()
endfunction()

set(LGP_COURSE_VERSION "course-v2026.09.05.3" CACHE STRING "Published course version" FORCE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

foreach(output_kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${output_kind}_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
endforeach()

include(CompilerFlags)
include(Dependencies)
include(ClangTools)

lgp_acquire_dependencies()

if(LGP_ENABLE_CLANG_TIDY)
    lgp_enable_clang_tidy()
endif()

add_subdirectory(framework)

if(LGP_BUILD_CHAPTERS)
    add_subdirectory(chapters)
endif()
