cmake_minimum_required(VERSION 3.20)
project(astra_startup_assets VERSION 1.0.0 LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "Startup asset preparation requires Windows BCrypt and path checks")
endif()
# MSVC 19.44 rejects mixed local/forwarded exports while generating an import
# library from object files. Build the export object separately with LIB, then
# link that object unchanged; this also preserves every original ordinal.
set(proxy_export_object "${CMAKE_CURRENT_BINARY_DIR}/proxy_exports.exp")
add_custom_command(OUTPUT "${proxy_export_object}"
  COMMAND "${CMAKE_AR}" /NOLOGO /MACHINE:X64
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/proxy_exports.def"
    "/OUT:${CMAKE_CURRENT_BINARY_DIR}/proxy_exports.lib"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proxy_exports.def"
  VERBATIM)
set_source_files_properties("${proxy_export_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
add_library(astra_rtx_startup_proxy SHARED proxy.cpp "${proxy_export_object}")
set_target_properties(astra_rtx_startup_proxy PROPERTIES OUTPUT_NAME d3d9 PREFIX "")
target_link_libraries(astra_rtx_startup_proxy PRIVATE astra_startup_assets shell32)
target_compile_features(astra_rtx_startup_proxy PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(astra_rtx_startup_proxy PRIVATE /W4 /WX /permissive- /EHsc)
endif()
add_library(astra_startup_assets STATIC startup_assets.cpp)
target_compile_features(astra_startup_assets PUBLIC cxx_std_17)
target_include_directories(astra_startup_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(astra_startup_assets PRIVATE bcrypt)
add_executable(astra_rtx_prepare_assets prepare_main.cpp)
target_link_libraries(astra_rtx_prepare_assets PRIVATE astra_startup_assets)
target_compile_features(astra_rtx_prepare_assets PRIVATE cxx_std_17)
foreach(target astra_startup_assets astra_rtx_prepare_assets)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
  endif()
endforeach()
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME startup_asset_contracts COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_startup_assets.py" "$<TARGET_FILE:astra_rtx_prepare_assets>")
  set(fake_exports "${CMAKE_CURRENT_BINARY_DIR}/fake_renderer.def")
  add_custom_command(OUTPUT "${fake_exports}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_fake_exports.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/proxy_renderer_exports.json" "${fake_exports}"
    DEPENDS tests/generate_fake_exports.py proxy_renderer_exports.json VERBATIM)
  add_library(astra_startup_fake_renderer SHARED tests/fake_renderer.cpp "${fake_exports}")
  set_target_properties(astra_startup_fake_renderer PROPERTIES OUTPUT_NAME d3d9_astra_renderer PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_renderer")
  target_compile_features(astra_startup_fake_renderer PRIVATE cxx_std_17)
  add_test(NAME startup_proxy_contracts COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_proxy.py" "$<TARGET_FILE:astra_rtx_startup_proxy>"
    "$<TARGET_FILE:astra_startup_fake_renderer>" "${CMAKE_CURRENT_SOURCE_DIR}/proxy_renderer_exports.json")
endif()
