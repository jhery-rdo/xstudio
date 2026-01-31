SET(LINK_DEPS
	xstudio::conform
	xstudio::utility
)

create_plugin_with_alias(conform_rodeo xstudio::conform::rodeo ${XSTUDIO_GLOBAL_VERSION} "${LINK_DEPS}")
