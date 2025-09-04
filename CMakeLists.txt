cmake_minimum_required(VERSION 3.20)
project(PlantUmlWebView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(PlantUmlWebView SHARED
    src/plantuml_wlx_ev2.cpp
)

# Produce a .wlx64 (it's still a normal DLL, just with a different extension)
set_target_properties(PlantUmlWebView PROPERTIES
    OUTPUT_NAME "PlantUmlWebView"
    SUFFIX ".wlx64"
)

target_compile_definitions(PlantUmlWebView PRIVATE UNICODE _UNICODE NOMINMAX)

# Headers are from Windows SDK + WebView2 SDK header.
# We load WebView2Loader.dll dynamically, so no import lib is needed.
target_include_directories(PlantUmlWebView PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(PlantUmlWebView PRIVATE
    user32.lib
    gdi32.lib
    ole32.lib
    shlwapi.lib
)

install(TARGETS PlantUmlWebView RUNTIME DESTINATION .)
install(FILES plantumlwebview.ini DESTINATION .)
install(FILES resources/pluginst.inf DESTINATION .)
install(FILES README.md DESTINATION .)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
