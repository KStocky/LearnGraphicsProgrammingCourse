include_guard(GLOBAL)

add_library(lgp_compiler_flags INTERFACE)
add_library(LGP::CompilerFlags ALIAS lgp_compiler_flags)

target_compile_features(lgp_compiler_flags INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(
        lgp_compiler_flags
        INTERFACE
            /W4
            /EHsc
            /permissive-
            /Zc:__cplusplus
            $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
            /utf-8
            $<$<BOOL:${LGP_WARNINGS_AS_ERRORS}>:/WX>)
else()
    message(FATAL_ERROR "Use MSVC or clang-cl with the MSVC-compatible frontend.")
endif()
