# Use weak jack library linking
if(LMMS_HAVE_WEAKJACK)
	# The pinned weakjack predates APIs used by native MIDI recovery. Extend a
	# build-local copy to retain runtime linking without changing the submodule.
	configure_file(weakjack/weak_libjack.c weak_libjack.c COPYONLY)
	configure_file(weakjack/weak_libjack.def weak_libjack.def COPYONLY)
	file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/weak_libjack.def" [[

JPFUN(1, int, port_connected_to, (const jack_port_t *p, const char *n), (p, n), -1)
JPFUN(1, uint32_t, midi_get_lost_event_count, (void *b), (b), ((uint32_t)-1))
]])
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/weakjack/weak_libjack.h" WEAKJACK_HEADER)
	file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/weak_libjack.h" "#ifdef USE_WEAK_JACK\n#define jack_port_connected_to WJACK_port_connected_to\n#define jack_midi_get_lost_event_count WJACK_midi_get_lost_event_count\n#endif\n${WEAKJACK_HEADER}")
	add_library(weakjack STATIC
		"${CMAKE_CURRENT_BINARY_DIR}/weak_libjack.c"
	)
	target_include_directories(weakjack PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
	target_link_libraries(weakjack PUBLIC jack_headers ${CMAKE_DL_LIBS})
	target_compile_features(weakjack PRIVATE c_std_11)
	# Enable weakjack, disable metadata support
	target_compile_definitions(weakjack PUBLIC
		USE_WEAK_JACK=1
		NO_JACK_METADATA=1
	)
endif()
