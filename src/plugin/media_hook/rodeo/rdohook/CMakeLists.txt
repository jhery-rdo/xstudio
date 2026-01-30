# Standalone CMakeLists.txt for building rodeo plugin separately
# This allows faster iteration when xStudio is already built
cmake_minimum_required(VERSION 3.16)
project(rdohook VERSION 1.0.0)

# Find xStudio (provides xstudio::media_hook, xstudio::utility, xstudio::module)
find_package(xStudio REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

# Build the plugin as a shared module
add_library(media_hook_rodeo MODULE
    src/rdohook.cpp
)

target_link_libraries(media_hook_rodeo PRIVATE
    xstudio::media_hook
    xstudio::utility
    xstudio::module
    OpenColorIO::OpenColorIO
    spdlog::spdlog
    fmt::fmt
)

target_include_directories(media_hook_rodeo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Plugin naming convention for xStudio
set_target_properties(media_hook_rodeo PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Install to xStudio plugin directory
install(TARGETS media_hook_rodeo
    LIBRARY DESTINATION share/xstudio/plugin/media_hook
)

# Install preference file
install(FILES src/plugin_rodeo_media_hook.json
    DESTINATION share/xstudio/plugin/media_hook
)
