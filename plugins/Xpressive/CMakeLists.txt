include(BuildPlugin)

add_library(exprtk INTERFACE)
target_include_directories(exprtk INTERFACE exprtk)
set_target_properties(exprtk PROPERTIES SYSTEM TRUE)
target_compile_definitions(exprtk INTERFACE
	exprtk_disable_sc_andor exprtk_disable_return_statement
	exprtk_disable_break_continue exprtk_disable_comments
	exprtk_disable_string_capabilities exprtk_disable_rtl_io_file
	exprtk_disable_rtl_vecops)
if(NOT MSVC)
	target_compile_options(exprtk INTERFACE -fexceptions)
endif()
if(LMMS_BUILD_WIN32 OR LMMS_BUILD_CYGWIN)
	if(MSVC)
		target_compile_options(exprtk INTERFACE /bigobj)
	else()
		target_compile_options(exprtk INTERFACE -Wa,-mbig-obj)
		target_compile_definitions(exprtk INTERFACE exprtk_disable_enhanced_features)
	endif()
endif()

build_plugin(xpressive
	Xpressive.cpp
	ExprSynth.cpp
	Xpressive.h
	ExprSynth.h
	MOCFILES Xpressive.h
	EMBEDDED_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.png"
)
target_link_libraries(xpressive exprtk)
