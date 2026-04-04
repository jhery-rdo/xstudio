# CMakeLists.txt for rodeo media loader plugin
# Supports both integrated build (as part of xStudio) and standalone build

# Check if we're building as part of xStudio (XSTUDIO_GLOBAL_VERSION is set)
if(DEFINED XSTUDIO_GLOBAL_VERSION)
    return()
endif()

# Standalone CMakeLists.txt for building the plugin separately
cmake_minimum_required(VERSION 3.16)
project(rdo_media_loader VERSION 1.0.0)

# Find xStudio
find_package(xStudio REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

# Build the plugin as a shared module
add_library(rdo_media_loader MODULE
    src/rdo_media_loader.cpp
)

target_link_libraries(rdo_media_loader PRIVATE
    xstudio::media_hook
    xstudio::media
    xstudio::utility
    xstudio::module
    xstudio::plugin_manager
    spdlog::spdlog
    fmt::fmt
)

target_include_directories(rdo_media_loader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Plugin naming convention for xStudio
set_target_properties(rdo_media_loader PROPERTIES
    PREFIX ""
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Cross-platform: use .so on Linux, .dylib on macOS
if(APPLE)
    set_target_properties(rdo_media_loader PROPERTIES SUFFIX ".dylib")
else()
    set_target_properties(rdo_media_loader PROPERTIES SUFFIX ".so")
endif()

# Install to xStudio plugin directory
install(TARGETS rdo_media_loader
    LIBRARY DESTINATION share/xstudio/plugin
)
