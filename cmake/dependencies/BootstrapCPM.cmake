include_guard(GLOBAL)

set(LGP_CPM_VERSION "0.43.1")
set(LGP_CPM_URL "https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.43.1/CPM.cmake")
set(LGP_CPM_SHA256 "1c40fc102ce9625d7de7eb14f541cab30cc3138dca627f0b0ec40293ce6c2934")

function(lgp_resolve_cpm_script out_var)
    set(_lgp_cpm_directory "${CMAKE_BINARY_DIR}/_deps/cpm")
    set(_lgp_cpm_path "${_lgp_cpm_directory}/CPM-${LGP_CPM_VERSION}.cmake")

    file(MAKE_DIRECTORY "${_lgp_cpm_directory}")

    set(_lgp_download_required YES)
    if(EXISTS "${_lgp_cpm_path}")
        file(SHA256 "${_lgp_cpm_path}" _lgp_existing_hash)
        if(_lgp_existing_hash STREQUAL LGP_CPM_SHA256)
            set(_lgp_download_required NO)
        endif()
    endif()

    if(_lgp_download_required)
        file(
            DOWNLOAD
            "${LGP_CPM_URL}"
            "${_lgp_cpm_path}"
            EXPECTED_HASH "SHA256=${LGP_CPM_SHA256}"
            TLS_VERIFY ON
            STATUS _lgp_download_status)

        list(GET _lgp_download_status 0 _lgp_download_status_code)
        if(NOT _lgp_download_status_code EQUAL 0)
            list(GET _lgp_download_status 1 _lgp_download_status_message)
            message(
                FATAL_ERROR
                "Failed to download CPM.cmake ${LGP_CPM_VERSION}: ${_lgp_download_status_message}")
        endif()
    endif()

    set("${out_var}" "${_lgp_cpm_path}" PARENT_SCOPE)
endfunction()
