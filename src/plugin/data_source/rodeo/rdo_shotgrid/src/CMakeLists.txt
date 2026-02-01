project(rdo_shotgrid VERSION ${XSTUDIO_GLOBAL_VERSION} LANGUAGES CXX)

set(SOURCES
    rdo_shotgrid_datasource.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})
add_library(xstudio::data_source::rdo_shotgrid ALIAS ${PROJECT_NAME})
default_plugin_options(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        xstudio::module
        xstudio::plugin_manager
        xstudio::data_source
)

set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS_NO_SHARED true)
