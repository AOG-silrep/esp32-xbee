idf_component_register(SRCS "f9p_config.c" "main.c"
		"config.c"
		"core_dump.c"
		"f9p_config.c"
		"geodesy.c"
		"log.c"
		"interface/ntrip_util.c"
		"retry.c"
		"rtk_status.c"
		"status_led.c"
		"stream_stats.c"
		"uart.c"
		"util.c"
		"web_server.c"
		"wifi.c"
		"interface/ntrip_caster.c"
		"interface/ntrip_client.c"
		"interface/ntrip_server.c"
		"interface/socket_client.c"
		"interface/socket_server.c"
		"protocol/nmea.c"
		"protocol/rtcm.c"
        INCLUDE_DIRS "include")

# Web UI is gzipped into the firmware, so one OTA updates both
file(GLOB WWW_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../www/*)
set(WWW_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/www_files.c)
idf_build_get_property(python PYTHON)
add_custom_command(OUTPUT ${WWW_SOURCE}
        COMMAND ${python} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/www_embed.py ${WWW_SOURCE} ${WWW_FILES}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/www_embed.py ${WWW_FILES}
        VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${WWW_SOURCE})
