cmake_minimum_required(VERSION 3.24)
project(PlantUmlWebView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- stable outputs into <repo>/dist ---
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY  ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY  ${CMAKE_SOURCE_DIR}/dist)

# build the WLX as a MODULE so it produces a single DLL
add_library(PlantUmlWebView MODULE
    src/plantuml_wlx_ev2.cpp
)

target_compile_features(PlantUmlWebView PRIVATE cxx_std_17)
target_compile_definitions(PlantUmlWebView PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(PlantUmlWebView PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/WebView2/build/native/include
)
target_link_libraries(PlantUmlWebView PRIVATE shlwapi)

# Name it exactly as TC expects and use the .wlx64 extension
set_target_properties(PlantUmlWebView PROPERTIES
    OUTPUT_NAME "PlantUmlWebView"
    PREFIX ""                 # no "lib" prefix anywhere
    SUFFIX ".wlx64"           # produce PlantUmlWebView.wlx64 instead of .dll
)

