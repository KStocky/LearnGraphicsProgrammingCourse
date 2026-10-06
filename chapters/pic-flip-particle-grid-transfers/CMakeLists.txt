add_library(ch43_transfer_contracts STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransferContracts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/TransferContracts.hpp")
target_link_libraries(ch43_transfer_contracts PUBLIC LGP::CompilerFlags)
target_include_directories(ch43_transfer_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Common")
add_library(ch43_transfer_starter_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Renderer.hpp")
target_compile_definitions(ch43_transfer_starter_logic PRIVATE LGP_TRANSFER_VARIANT=starter)
target_include_directories(ch43_transfer_starter_logic BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ch43_transfer_starter_logic PUBLIC ch43_transfer_contracts LGP::Framework)
add_library(ch43_transfer_solution_logic STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Renderer.hpp")
target_compile_definitions(ch43_transfer_solution_logic PRIVATE LGP_TRANSFER_VARIANT=solution)
target_include_directories(ch43_transfer_solution_logic BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ch43_transfer_solution_logic PUBLIC ch43_transfer_contracts LGP::Framework)
add_executable(Ch43_PicFlipTransfers_Starter "${CMAKE_CURRENT_SOURCE_DIR}/Starter/Main.cpp")
target_compile_definitions(Ch43_PicFlipTransfers_Starter PRIVATE LGP_TRANSFER_VARIANT=starter)
lgp_configure_framework_chapter_app(TARGET Ch43_PicFlipTransfers_Starter EXTRA_DEPENDENCIES ch43_transfer_starter_logic)
add_executable(Ch43_PicFlipTransfers_Solution "${CMAKE_CURRENT_SOURCE_DIR}/Solution/Main.cpp")
target_compile_definitions(Ch43_PicFlipTransfers_Solution PRIVATE LGP_TRANSFER_VARIANT=solution)
lgp_configure_framework_chapter_app(TARGET Ch43_PicFlipTransfers_Solution EXTRA_DEPENDENCIES ch43_transfer_solution_logic)
