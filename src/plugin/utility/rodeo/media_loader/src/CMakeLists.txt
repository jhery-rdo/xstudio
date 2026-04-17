SET(LINK_DEPS
	xstudio::media_hook
	xstudio::media
	xstudio::utility
	xstudio::module
	xstudio::plugin_manager
	xstudio::http_client
)

create_plugin_with_alias(rdo_media_loader xstudio::plugin::rdo_media_loader ${XSTUDIO_GLOBAL_VERSION} "${LINK_DEPS}")

# cpp-httplib SSL + zlib support for downloading Web (sg_uploaded_movie_mp4) URLs.
target_compile_definitions(rdo_media_loader PRIVATE
	CPPHTTPLIB_OPENSSL_SUPPORT
	CPPHTTPLIB_ZLIB_SUPPORT
)
