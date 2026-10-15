SET(PLUGIN_FILES "")
IF(LMMS_BUILD_WIN32)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QWindowsIntegrationPlugin> DESTINATION platforms)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QSvgIconPlugin> DESTINATION iconengines)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QSvgPlugin> DESTINATION imageformats)
        INSTALL(FILES $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Svg> DESTINATION .)
	# Qt 6 loads TLS backends as plugins. The existing executable dependency
	# scan cannot discover dynamically loaded backend DLLs.
	if(QT_VERSION_MAJOR EQUAL 6)
		set(LMMS_TLS_BACKENDS "")
		foreach(backend QTlsBackendSchannelPlugin QTlsBackendOpenSSLPlugin)
			if(TARGET Qt6::${backend})
				get_target_property(backend_type Qt6::${backend} TYPE)
				if(NOT backend_type STREQUAL "STATIC_LIBRARY")
					install(FILES $<TARGET_FILE:Qt6::${backend}> DESTINATION tls)
					list(APPEND LMMS_TLS_BACKENDS Qt6::${backend})
				endif()
			endif()
		endforeach()
	endif()
ENDIF()

IF(LMMS_BUILD_WIN32 OR LMMS_INSTALL_DEPENDENCIES)
	include(InstallTargetDependencies)

	# Collect directories to search for DLLs
	GET_FILENAME_COMPONENT(QTBIN_DIR "${QT_QMAKE_EXECUTABLE}" PATH)
	set(LIB_DIRS "${QTBIN_DIR}")

	GET_PROPERTY(PLUGINS_BUILT GLOBAL PROPERTY PLUGINS_BUILT)

	IF(LMMS_BUILD_WIN32)
		SET(LMMS_DEP_DESTINATION ${BIN_DIR})
		SET(PLUGIN_DEP_DESTINATION ${BIN_DIR})
	ELSE()
		SET(LMMS_DEP_DESTINATION ${LIB_DIR})
		SET(PLUGIN_DEP_DESTINATION ${LIB_DIR})
	ENDIF()

	INSTALL_TARGET_DEPENDENCIES(
			NAME "main_binary"
			TARGETS lmms
			DESTINATION "${LMMS_DEP_DESTINATION}"
			LIB_DIRS  ${LIB_DIRS}
	)

	INSTALL_TARGET_DEPENDENCIES(
			NAME "plugins"
			TARGETS ${PLUGINS_BUILT}
			DESTINATION ${PLUGIN_DEP_DESTINATION}
			LIB_DIRS ${LIB_DIRS} "${PLUGIN_DIR}" "${PLUGIN_DIR}/optional"
			SEARCH_PATHS "${PLUGIN_DIR}" "${PLUGIN_DIR}/optional"
	)
	if(LMMS_TLS_BACKENDS)
		INSTALL_TARGET_DEPENDENCIES(
				NAME "tls_backends"
				TARGETS ${LMMS_TLS_BACKENDS}
				DESTINATION "${LMMS_DEP_DESTINATION}"
				LIB_DIRS ${LIB_DIRS}
		)
	endif()
ENDIF()

# Install STK rawwaves
if(LMMS_HAVE_STK AND (LMMS_BUILD_WIN32 OR LMMS_BUILD_APPLE))
	if(STK_RAWWAVE_ROOT)
		file(GLOB RAWWAVES "${STK_RAWWAVE_ROOT}/*.raw")
		list(SORT RAWWAVES)
		install(FILES ${RAWWAVES} DESTINATION "${DATA_DIR}/stk/rawwaves")
	else()
		message(WARNING "Can't find STK rawwave root!")
	endif()
endif()

