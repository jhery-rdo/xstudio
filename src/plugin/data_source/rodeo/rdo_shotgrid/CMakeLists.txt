# Standalone CMakeLists.txt for building rodeo shotgrid data source plugin separately
# This allows faster iteration when xStudio is already built
cmake_minimum_required(VERSION 3.16)
project(rdo_shotgrid VERSION 1.0.0)

# Find xStudio (provides xstudio::data_source, xstudio::module, etc.)
find_package(xStudio REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

# Build the plugin as a shared module
add_library(rdo_shotgrid MODULE
    src/rdo_shotgrid_datasource.cpp
)

target_link_libraries(rdo_shotgrid PRIVATE
    xstudio::data_source
    xstudio::module
    xstudio::plugin_manager
    spdlog::spdlog
    fmt::fmt
)

target_include_directories(rdo_shotgrid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Plugin naming convention for xStudio
set_target_properties(rdo_shotgrid PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Install to xStudio plugin directory (for make install)
install(TARGETS rdo_shotgrid
    LIBRARY DESTINATION share/xstudio/plugin
)
