SET(LINK_DEPS
	xstudio::media_hook
	xstudio::media
	xstudio::utility
	xstudio::module
	xstudio::plugin_manager
)

create_plugin_with_alias(rdo_media_loader xstudio::plugin::rdo_media_loader ${XSTUDIO_GLOBAL_VERSION} "${LINK_DEPS}")
