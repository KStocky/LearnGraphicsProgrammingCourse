add_library(ch42_sph_contracts STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SphContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/SphContracts.hpp")
target_link_libraries(ch42_sph_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch42_sph_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")

add_library(ch42_sph_starter_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
target_compile_definitions(ch42_sph_starter_logic PRIVATE LGP_SPH_VARIANT=starter)
target_include_directories(ch42_sph_starter_logic BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ch42_sph_starter_logic PUBLIC ch42_sph_contracts LGP::Framework)
add_library(ch42_sph_solution_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")
target_compile_definitions(ch42_sph_solution_logic PRIVATE LGP_SPH_VARIANT=solution)
target_include_directories(ch42_sph_solution_logic BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ch42_sph_solution_logic PUBLIC ch42_sph_contracts LGP::Framework)
add_executable(Ch42_SphParticleFluid_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
target_compile_definitions(Ch42_SphParticleFluid_Starter PRIVATE LGP_SPH_VARIANT=starter)
lgp_configure_framework_chapter_app(TARGET Ch42_SphParticleFluid_Starter EXTRA_DEPENDENCIES ch42_sph_starter_logic)
add_executable(Ch42_SphParticleFluid_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_compile_definitions(Ch42_SphParticleFluid_Solution PRIVATE LGP_SPH_VARIANT=solution)
lgp_configure_framework_chapter_app(TARGET Ch42_SphParticleFluid_Solution EXTRA_DEPENDENCIES ch42_sph_solution_logic)
