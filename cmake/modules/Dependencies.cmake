include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/../dependencies/BootstrapCPM.cmake")

set(LGP_DIRECTXMATH_VERSION "jun2026")
set(LGP_DIRECTXMATH_URL "https://github.com/microsoft/DirectXMath/archive/refs/tags/jun2026.zip")
set(LGP_DIRECTXMATH_SHA256 "d1595ddbb53583485126968465a7a6a770c8280c4519ba5d7a69af34a9ab4fe6")

set(LGP_IMGUI_VERSION "v1.92.9b")
set(LGP_IMGUI_URL "https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip")
set(LGP_IMGUI_SHA256 "e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8")

set(LGP_CATCH2_VERSION "v3.15.3")
set(LGP_CATCH2_URL "https://github.com/catchorg/Catch2/archive/refs/tags/v3.15.3.zip")
set(LGP_CATCH2_SHA256 "d36d45ec0dbc3d26936dfab665695f08eed87c253d12546c135074273e89b755")

set(LGP_D3D12_AGILITY_VERSION "1.619.5")
set(LGP_D3D12_AGILITY_SDK_VERSION 619)
set(LGP_D3D12_AGILITY_URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.619.5/microsoft.direct3d.d3d12.1.619.5.nupkg")
set(LGP_D3D12_AGILITY_SHA256 "0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6")

set(LGP_DXC_VERSION "1.9.2607.13")
set(LGP_DXC_URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.dxc/1.9.2607.13/microsoft.direct3d.dxc.1.9.2607.13.nupkg")
set(LGP_DXC_SHA256 "5d6acd23089b2979a3c1d39b7e31227da989a47b5d9f3db57111ad4717ea537e")

set(LGP_WINPIXEVENTRUNTIME_VERSION "1.0.240308001")
set(LGP_WINPIXEVENTRUNTIME_URL "https://api.nuget.org/v3-flatcontainer/winpixeventruntime/1.0.240308001/winpixeventruntime.1.0.240308001.nupkg")
set(LGP_WINPIXEVENTRUNTIME_SHA256 "726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4")

set(LGP_WARP_VERSION "1.0.20")
set(LGP_WARP_URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/1.0.20/microsoft.direct3d.warp.1.0.20.nupkg")
set(LGP_WARP_SHA256 "e5fe5de661ce98b58ef9cfb736e73c0a7a2623d3bbf5f14839b2d55566d87e40")

function(_lgp_require_dependency_path dependency path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Pinned dependency ${dependency} is missing ${path}.")
    endif()
endfunction()

function(_lgp_get_nuget_x64_path out_var source_dir relative_path)
    string(REPLACE "/" ";" _lgp_relative_components "${relative_path}")
    cmake_path(APPEND _lgp_absolute_path "${source_dir}" ${_lgp_relative_components})
    set("${out_var}" "${_lgp_absolute_path}" PARENT_SCOPE)
endfunction()

function(_lgp_escape_windows_relative_path out_stage_subdirectory out_export_literal input_path)
    if(NOT input_path)
        message(FATAL_ERROR "The Agility SDK path must not be empty.")
    endif()

    string(REPLACE "/" "\\" _lgp_subdirectory "${input_path}")
    string(REGEX REPLACE "^[.\\\\]+" "" _lgp_subdirectory "${_lgp_subdirectory}")
    string(REGEX REPLACE "\\\\+$" "" _lgp_subdirectory "${_lgp_subdirectory}")

    if(_lgp_subdirectory STREQUAL "")
        message(FATAL_ERROR "The Agility SDK path must resolve to a relative subdirectory.")
    endif()

    string(REPLACE "\\" "\\\\" _lgp_export_subdirectory "${_lgp_subdirectory}")

    set("${out_stage_subdirectory}" "${_lgp_subdirectory}" PARENT_SCOPE)
    set("${out_export_literal}" ".\\\\${_lgp_export_subdirectory}\\\\" PARENT_SCOPE)
endfunction()

function(_lgp_add_directxmath_target source_dir)
    if(TARGET Microsoft::DirectXMath)
        return()
    endif()

    set(_lgp_include_dir "${source_dir}/Inc")
    _lgp_require_dependency_path("Microsoft::DirectXMath" "${_lgp_include_dir}")

    add_library(Microsoft::DirectXMath INTERFACE IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::DirectXMath
        PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_lgp_include_dir}"
            INTERFACE_COMPILE_FEATURES "cxx_std_11"
            LGP_PACKAGE_VERSION "${LGP_DIRECTXMATH_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_DIRECTXMATH_SHA256}")
endfunction()

function(_lgp_add_imgui_target source_dir)
    if(TARGET DearImGui::DearImGui)
        return()
    endif()

    set(
        _lgp_imgui_sources
        "${source_dir}/imgui.cpp"
        "${source_dir}/imgui_draw.cpp"
        "${source_dir}/imgui_tables.cpp"
        "${source_dir}/imgui_widgets.cpp")

    foreach(_lgp_source IN LISTS _lgp_imgui_sources)
        _lgp_require_dependency_path("DearImGui::DearImGui" "${_lgp_source}")
    endforeach()

    add_library(lgp_dear_imgui STATIC ${_lgp_imgui_sources})
    add_library(DearImGui::DearImGui ALIAS lgp_dear_imgui)

    target_include_directories(lgp_dear_imgui SYSTEM PUBLIC "${source_dir}")

    if(MSVC)
        target_compile_options(lgp_dear_imgui PRIVATE /W0)
    endif()

    set_target_properties(
        lgp_dear_imgui
        PROPERTIES
            FOLDER "ThirdParty"
            SYSTEM TRUE)
endfunction()

function(_lgp_add_agility_sdk_target source_dir)
    if(TARGET Microsoft::Direct3D12AgilitySDK)
        return()
    endif()

    _lgp_get_nuget_x64_path(_lgp_include_dir "${source_dir}" "build/native/include")
    _lgp_get_nuget_x64_path(_lgp_runtime_dir "${source_dir}" "build/native/bin/x64")

    set(
        _lgp_runtime_files
        "${_lgp_runtime_dir}/D3D12Core.dll"
        "${_lgp_runtime_dir}/d3d12SDKLayers.dll"
        "${_lgp_runtime_dir}/D3D12StateObjectCompiler.dll")

    _lgp_require_dependency_path("Microsoft::Direct3D12AgilitySDK" "${_lgp_include_dir}")
    foreach(_lgp_runtime_file IN LISTS _lgp_runtime_files)
        _lgp_require_dependency_path("Microsoft::Direct3D12AgilitySDK" "${_lgp_runtime_file}")
    endforeach()

    add_library(Microsoft::Direct3D12AgilitySDK INTERFACE IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::Direct3D12AgilitySDK
        PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_lgp_include_dir}"
            LGP_AGILITY_SDK_VERSION "${LGP_D3D12_AGILITY_SDK_VERSION}"
            LGP_PACKAGE_VERSION "${LGP_D3D12_AGILITY_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_D3D12_AGILITY_SHA256}"
            LGP_RUNTIME_FILES "${_lgp_runtime_files}"
            LGP_RUNTIME_SUBDIRECTORY "D3D12")
endfunction()

function(_lgp_add_dxc_targets source_dir)
    if(TARGET Microsoft::DXC)
        return()
    endif()

    _lgp_get_nuget_x64_path(_lgp_include_dir "${source_dir}" "build/native/include")
    _lgp_get_nuget_x64_path(_lgp_runtime_dir "${source_dir}" "build/native/bin/x64")
    _lgp_get_nuget_x64_path(_lgp_library_dir "${source_dir}" "build/native/lib/x64")

    set(_lgp_dxcompiler_dll "${_lgp_runtime_dir}/dxcompiler.dll")
    set(_lgp_dxcompiler_lib "${_lgp_library_dir}/dxcompiler.lib")
    set(_lgp_dxil_dll "${_lgp_runtime_dir}/dxil.dll")
    set(_lgp_dxil_lib "${_lgp_library_dir}/dxil.lib")
    set(_lgp_dxc_executable "${_lgp_runtime_dir}/dxc.exe")

    foreach(
        _lgp_required_path
        IN ITEMS
            "${_lgp_include_dir}"
            "${_lgp_dxcompiler_dll}"
            "${_lgp_dxcompiler_lib}"
            "${_lgp_dxil_dll}"
            "${_lgp_dxil_lib}"
            "${_lgp_dxc_executable}")
        _lgp_require_dependency_path("Microsoft::DXC" "${_lgp_required_path}")
    endforeach()

    add_library(Microsoft::DXCompiler SHARED IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::DXCompiler
        PROPERTIES
            IMPORTED_IMPLIB "${_lgp_dxcompiler_lib}"
            IMPORTED_LOCATION "${_lgp_dxcompiler_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${_lgp_include_dir}"
            LGP_PACKAGE_VERSION "${LGP_DXC_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_DXC_SHA256}"
            LGP_RUNTIME_FILES "${_lgp_dxcompiler_dll}")

    add_library(Microsoft::DXIL SHARED IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::DXIL
        PROPERTIES
            IMPORTED_IMPLIB "${_lgp_dxil_lib}"
            IMPORTED_LOCATION "${_lgp_dxil_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${_lgp_include_dir}"
            LGP_PACKAGE_VERSION "${LGP_DXC_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_DXC_SHA256}"
            LGP_RUNTIME_FILES "${_lgp_dxil_dll}")

    add_library(Microsoft::DXC INTERFACE IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::DXC
        PROPERTIES
            INTERFACE_LINK_LIBRARIES "Microsoft::DXCompiler;Microsoft::DXIL"
            LGP_PACKAGE_VERSION "${LGP_DXC_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_DXC_SHA256}"
            LGP_RUNTIME_FILES "${_lgp_dxcompiler_dll};${_lgp_dxil_dll}")

    add_executable(Microsoft::DXC::Compiler IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::DXC::Compiler
        PROPERTIES
            IMPORTED_LOCATION "${_lgp_dxc_executable}")
endfunction()

function(_lgp_add_winpix_target source_dir)
    if(TARGET Microsoft::WinPixEventRuntime)
        return()
    endif()

    _lgp_get_nuget_x64_path(_lgp_include_dir "${source_dir}" "Include/WinPixEventRuntime")
    _lgp_get_nuget_x64_path(_lgp_runtime_dir "${source_dir}" "bin/x64")

    set(_lgp_runtime_dll "${_lgp_runtime_dir}/WinPixEventRuntime.dll")
    set(_lgp_import_library "${_lgp_runtime_dir}/WinPixEventRuntime.lib")

    foreach(
        _lgp_required_path
        IN ITEMS
            "${_lgp_include_dir}"
            "${_lgp_runtime_dll}"
            "${_lgp_import_library}")
        _lgp_require_dependency_path("Microsoft::WinPixEventRuntime" "${_lgp_required_path}")
    endforeach()

    add_library(Microsoft::WinPixEventRuntime SHARED IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::WinPixEventRuntime
        PROPERTIES
            IMPORTED_IMPLIB "${_lgp_import_library}"
            IMPORTED_LOCATION "${_lgp_runtime_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${_lgp_include_dir}"
            LGP_PACKAGE_VERSION "${LGP_WINPIXEVENTRUNTIME_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_WINPIXEVENTRUNTIME_SHA256}"
            LGP_RUNTIME_FILES "${_lgp_runtime_dll}")
endfunction()

function(_lgp_add_warp_target source_dir)
    if(TARGET Microsoft::Direct3DWARP)
        return()
    endif()

    _lgp_get_nuget_x64_path(_lgp_runtime_file "${source_dir}" "build/native/bin/x64/d3d10warp.dll")
    _lgp_require_dependency_path("Microsoft::Direct3DWARP" "${_lgp_runtime_file}")
    _lgp_require_dependency_path("Microsoft::Direct3DWARP" "${source_dir}/LICENSE.TXT")

    add_library(Microsoft::Direct3DWARP INTERFACE IMPORTED GLOBAL)
    set_target_properties(
        Microsoft::Direct3DWARP
        PROPERTIES
            LGP_PACKAGE_VERSION "${LGP_WARP_VERSION}"
            LGP_PACKAGE_SHA256 "${LGP_WARP_SHA256}"
            LGP_RUNTIME_FILES "${_lgp_runtime_file}"
            LGP_TESTING_ONLY TRUE)
endfunction()

function(lgp_stage_runtime_dependencies)
    set(options)
    set(oneValueArgs TARGET DESTINATION_SUBDIRECTORY)
    set(multiValueArgs DEPENDENCIES)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "lgp_stage_runtime_dependencies requires TARGET.")
    endif()

    if(NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR "lgp_stage_runtime_dependencies target ${ARG_TARGET} does not exist.")
    endif()

    foreach(_lgp_dependency IN LISTS ARG_DEPENDENCIES)
        if(NOT TARGET "${_lgp_dependency}")
            message(FATAL_ERROR "Runtime dependency target ${_lgp_dependency} does not exist.")
        endif()

        get_target_property(_lgp_runtime_files "${_lgp_dependency}" LGP_RUNTIME_FILES)
        if(NOT _lgp_runtime_files OR _lgp_runtime_files STREQUAL "_lgp_runtime_files-NOTFOUND")
            get_target_property(_lgp_imported_location "${_lgp_dependency}" IMPORTED_LOCATION)
            if(_lgp_imported_location AND NOT _lgp_imported_location STREQUAL "_lgp_imported_location-NOTFOUND")
                set(_lgp_runtime_files "${_lgp_imported_location}")
            else()
                message(FATAL_ERROR "Runtime dependency ${_lgp_dependency} does not declare runtime files.")
            endif()
        endif()

        if(ARG_DESTINATION_SUBDIRECTORY)
            set(_lgp_runtime_subdirectory "${ARG_DESTINATION_SUBDIRECTORY}")
        else()
            get_target_property(_lgp_runtime_subdirectory "${_lgp_dependency}" LGP_RUNTIME_SUBDIRECTORY)
        endif()
        set(_lgp_destination_directory "$<TARGET_FILE_DIR:${ARG_TARGET}>")
        if(_lgp_runtime_subdirectory AND NOT _lgp_runtime_subdirectory STREQUAL "_lgp_runtime_subdirectory-NOTFOUND")
            string(REPLACE "\\" "/" _lgp_runtime_subdirectory "${_lgp_runtime_subdirectory}")
            set(_lgp_destination_directory "${_lgp_destination_directory}/${_lgp_runtime_subdirectory}")
        endif()

        add_custom_command(
            TARGET "${ARG_TARGET}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_lgp_destination_directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_lgp_runtime_files} "${_lgp_destination_directory}"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endforeach()
endfunction()

function(lgp_add_d3d12_agility_sdk_exports)
    set(options)
    set(oneValueArgs TARGET SDK_PATH)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "lgp_add_d3d12_agility_sdk_exports requires TARGET.")
    endif()

    if(NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR "Agility SDK export target ${ARG_TARGET} does not exist.")
    endif()

    if(NOT TARGET Microsoft::Direct3D12AgilitySDK)
        message(FATAL_ERROR "Microsoft::Direct3D12AgilitySDK has not been acquired.")
    endif()

    get_target_property(_lgp_agility_sdk_version Microsoft::Direct3D12AgilitySDK LGP_AGILITY_SDK_VERSION)
    if(NOT _lgp_agility_sdk_version OR _lgp_agility_sdk_version STREQUAL "_lgp_agility_sdk_version-NOTFOUND")
        message(FATAL_ERROR "Microsoft::Direct3D12AgilitySDK does not declare an SDK version.")
    endif()

    if(ARG_SDK_PATH)
        set(_lgp_agility_sdk_path "${ARG_SDK_PATH}")
    else()
        set(_lgp_agility_sdk_path "D3D12")
    endif()

    _lgp_escape_windows_relative_path(
        _lgp_stage_subdirectory
        _lgp_export_literal
        "${_lgp_agility_sdk_path}")

    string(MAKE_C_IDENTIFIER "${ARG_TARGET}" _lgp_target_identifier)
    string(SHA256 _lgp_target_hash "${_lgp_target_identifier}")
    string(SUBSTRING "${_lgp_target_hash}" 0 12 _lgp_target_hash)
    set(_lgp_generated_source "${CMAKE_CURRENT_BINARY_DIR}/lgp_agility_${_lgp_target_hash}.cpp")

    set(LGP_AGILITY_SDK_VERSION "${_lgp_agility_sdk_version}")
    set(LGP_AGILITY_SDK_PATH "${_lgp_export_literal}")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../dependencies/AgilitySDKExports.cpp.in"
        "${_lgp_generated_source}"
        @ONLY)

    target_sources("${ARG_TARGET}" PRIVATE "${_lgp_generated_source}")
endfunction()

function(lgp_enable_d3d12_agility_sdk)
    set(options)
    set(oneValueArgs TARGET SDK_PATH)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "lgp_enable_d3d12_agility_sdk requires TARGET.")
    endif()

    if(ARG_SDK_PATH)
        set(_lgp_agility_sdk_path "${ARG_SDK_PATH}")
    else()
        set(_lgp_agility_sdk_path "D3D12")
    endif()

    _lgp_escape_windows_relative_path(
        _lgp_stage_subdirectory
        _lgp_export_literal
        "${_lgp_agility_sdk_path}")

    target_link_libraries("${ARG_TARGET}" PRIVATE Microsoft::Direct3D12AgilitySDK)
    lgp_add_d3d12_agility_sdk_exports(TARGET "${ARG_TARGET}" SDK_PATH "${_lgp_agility_sdk_path}")
    lgp_stage_runtime_dependencies(
        TARGET "${ARG_TARGET}"
        DEPENDENCIES Microsoft::Direct3D12AgilitySDK
        DESTINATION_SUBDIRECTORY "${_lgp_stage_subdirectory}")
endfunction()

function(lgp_acquire_dependencies)
    if(TARGET Microsoft::Direct3D12AgilitySDK)
        return()
    endif()

    if(NOT WIN32)
        message(FATAL_ERROR "LearnGraphicsProgramming dependencies require Windows.")
    endif()

    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "LearnGraphicsProgramming dependencies require x64 builds.")
    endif()

    set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
    if(DEFINED ENV{CPM_SOURCE_CACHE} AND NOT "$ENV{CPM_SOURCE_CACHE}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{CPM_SOURCE_CACHE}" CPM_SOURCE_CACHE)
    else()
        set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/_deps/cache")
    endif()
    set(CPM_USE_LOCAL_PACKAGES OFF)
    set(CPM_LOCAL_PACKAGES_ONLY OFF)
    set(CPM_DONT_UPDATE_MODULE_PATH ON)
    set(CPM_USE_NAMED_CACHE_DIRECTORIES ON)

    lgp_resolve_cpm_script(_lgp_cpm_script)
    include("${_lgp_cpm_script}")

    message(
        STATUS
        "Acquiring pinned dependencies: DirectXMath ${LGP_DIRECTXMATH_VERSION}, Agility SDK ${LGP_D3D12_AGILITY_VERSION}, DXC ${LGP_DXC_VERSION}, WinPixEventRuntime ${LGP_WINPIXEVENTRUNTIME_VERSION}")

    CPMAddPackage(
        NAME directxmath
        URL "${LGP_DIRECTXMATH_URL}"
        URL_HASH "SHA256=${LGP_DIRECTXMATH_SHA256}"
        DOWNLOAD_ONLY YES)
    _lgp_add_directxmath_target("${directxmath_SOURCE_DIR}")

    if(LGP_ENABLE_IMGUI)
        CPMAddPackage(
            NAME imgui
            URL "${LGP_IMGUI_URL}"
            URL_HASH "SHA256=${LGP_IMGUI_SHA256}"
            DOWNLOAD_ONLY YES)
        _lgp_add_imgui_target("${imgui_SOURCE_DIR}")
    endif()

    if(LGP_BUILD_TESTS)
        CPMAddPackage(
            NAME catch2
            URL "${LGP_CATCH2_URL}"
            URL_HASH "SHA256=${LGP_CATCH2_SHA256}"
            SYSTEM YES
            OPTIONS
                "CATCH_BUILD_TESTING OFF"
                "CATCH_INSTALL_DOCS OFF"
                "CATCH_INSTALL_EXTRAS OFF"
                "CATCH_DEVELOPMENT_BUILD OFF"
                "CATCH_ENABLE_WERROR OFF")

        set_target_properties(Catch2 Catch2WithMain PROPERTIES FOLDER "ThirdParty")
    endif()

    CPMAddPackage(
        NAME d3d12agilitysdk
        URL "${LGP_D3D12_AGILITY_URL}"
        URL_HASH "SHA256=${LGP_D3D12_AGILITY_SHA256}"
        DOWNLOAD_ONLY YES)
    _lgp_add_agility_sdk_target("${d3d12agilitysdk_SOURCE_DIR}")

    CPMAddPackage(
        NAME dxc
        URL "${LGP_DXC_URL}"
        URL_HASH "SHA256=${LGP_DXC_SHA256}"
        DOWNLOAD_ONLY YES)
    _lgp_add_dxc_targets("${dxc_SOURCE_DIR}")

    CPMAddPackage(
        NAME winpixeventruntime
        URL "${LGP_WINPIXEVENTRUNTIME_URL}"
        URL_HASH "SHA256=${LGP_WINPIXEVENTRUNTIME_SHA256}"
        DOWNLOAD_ONLY YES)
    _lgp_add_winpix_target("${winpixeventruntime_SOURCE_DIR}")

    if(LGP_BUILD_TESTS)
        message(STATUS "Acquiring testing-only WARP ${LGP_WARP_VERSION}")
        CPMAddPackage(
            NAME direct3dwarp
            URL "${LGP_WARP_URL}"
            URL_HASH "SHA256=${LGP_WARP_SHA256}"
            DOWNLOAD_ONLY YES)
        _lgp_add_warp_target("${direct3dwarp_SOURCE_DIR}")
    endif()
endfunction()
