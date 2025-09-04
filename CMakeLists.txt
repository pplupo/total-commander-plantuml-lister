# CMakeLists.txt
cmake_minimum_required(VERSION 3.24)
project(PlantUmlWebView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build the Total Commander lister DLL
add_library(PlantUmlWebView SHARED
    src/plantuml_wlx_ev2.cpp
)

# WebView2 headers (matches the path your workflow copies from NuGet)
target_include_directories(PlantUmlWebView PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/WebView2/build/native/include
)

# Minimal defs; we do NOT link the WebView2 import lib (we LoadLibrary at runtime)
target_compile_definitions(PlantUmlWebView PRIVATE UNICODE _UNICODE NOMINMAX)

# Output .wlx64 into dist/
set_target_properties(PlantUmlWebView PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    OUTPUT_NAME "PlantUmlWebView"
)

# Copy the produced DLL to .wlx64 name (TC expects that extension)
add_custom_command(TARGET PlantUmlWebView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:PlantUmlWebView>"
            "${CMAKE_SOURCE_DIR}/dist/PlantUmlWebView.wlx64"
)
