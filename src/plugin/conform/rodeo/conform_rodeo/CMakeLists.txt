# Standalone CMakeLists.txt for building rodeo conform plugin separately
# This allows faster iteration when xStudio is already built
cmake_minimum_required(VERSION 3.16)
project(conform_rodeo VERSION 1.0.0)

# Find xStudio (provides xstudio::conform, xstudio::utility)
find_package(xStudio REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

# Build the plugin as a shared module
add_library(conform_rodeo MODULE
    src/conform_rodeo.cpp
)

target_link_libraries(conform_rodeo PRIVATE
    xstudio::conform
    xstudio::utility
    spdlog::spdlog
    fmt::fmt
)

target_include_directories(conform_rodeo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Plugin naming convention for xStudio
set_target_properties(conform_rodeo PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Install to xStudio plugin directory (for make install)
install(TARGETS conform_rodeo
    LIBRARY DESTINATION share/xstudio/plugin
)

# Install preference file
install(FILES share/preference/plugin_conformer_rodeo.json
    DESTINATION share/xstudio/preference
)
