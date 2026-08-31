cmake_minimum_required(VERSION 3.20)
project(advmat_rtx_bridge_writer VERSION 1.0.0 LANGUAGES CXX)

option(ADVMAT_RTX_BUILD_TESTS "Build the pure writer contract tests" ON)

function(advmat_rtx_strict_compile target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(advmat_rtx_writer STATIC
    src/bridge.cpp
    src/filesystem_utils.cpp
    src/texture_ingest.cpp
    src/usda_writer.cpp)
target_include_directories(advmat_rtx_writer PUBLIC include PRIVATE src)
target_compile_features(advmat_rtx_writer PUBLIC cxx_std_17)
set_target_properties(advmat_rtx_writer PROPERTIES POSITION_INDEPENDENT_CODE ON)
advmat_rtx_strict_compile(advmat_rtx_writer)

if(WIN32)
    target_link_libraries(advmat_rtx_writer PRIVATE windowscodecs ole32)
endif()

if(ADVMAT_RTX_BUILD_TESTS)
    enable_testing()
    add_executable(advmat_rtx_writer_tests tests/writer_tests.cpp)
    target_link_libraries(advmat_rtx_writer_tests PRIVATE advmat_rtx_writer)
    target_compile_features(advmat_rtx_writer_tests PRIVATE cxx_std_17)
    advmat_rtx_strict_compile(advmat_rtx_writer_tests)
    add_test(NAME advmat_rtx_writer_tests COMMAND advmat_rtx_writer_tests)
endif()
