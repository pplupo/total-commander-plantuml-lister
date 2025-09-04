cmake_minimum_required(VERSION 3.24)
project(PlantUmlWebView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(PlantUmlWebView SHARED
    src/plantuml_wlx_ev2.cpp
)

# Exported name expected by Total Commander
set_target_properties(PlantUmlWebView PROPERTIES
    OUTPUT_NAME "PlantUmlWebView"
    SUFFIX ".wlx64"
)

# WebView2 headers (your workflow downloads the SDK headers)
target_include_directories(PlantUmlWebView PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/WebView2/build/native/include
)

target_link_libraries(PlantUmlWebView PRIVATE
    shlwapi
)
