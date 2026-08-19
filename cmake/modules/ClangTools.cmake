include_guard(GLOBAL)

function(lgp_enable_clang_tidy)
    find_program(
        LGP_CLANG_TIDY
        NAMES clang-tidy
        HINTS "$ENV{ProgramFiles}/LLVM/bin"
        REQUIRED)

    set(CMAKE_CXX_CLANG_TIDY "${LGP_CLANG_TIDY}" PARENT_SCOPE)
endfunction()
