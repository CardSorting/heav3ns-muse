# If Carla was not provided by the system, make a dummy library instead
if(LMMS_HAVE_WEAKCARLA)
  SET(CARLA_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/carla/source
    ${CMAKE_CURRENT_SOURCE_DIR}/carla/source/includes
    ${CMAKE_CURRENT_SOURCE_DIR}/carla/source/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/carla/source/backend
  )

  # force "lib" prefix
  IF(LMMS_BUILD_WIN32)
      SET(CMAKE_SHARED_LIBRARY_PREFIX "lib")
  ENDIF()

  SET(CARLA_NATIVE_LIB carla_native-plugin)

  ADD_LIBRARY(${CARLA_NATIVE_LIB} SHARED DummyCarla.cpp)
  target_include_directories(${CARLA_NATIVE_LIB} SYSTEM PUBLIC ${CARLA_INCLUDE_DIRS})
  INSTALL(TARGETS ${CARLA_NATIVE_LIB}
    LIBRARY DESTINATION "${PLUGIN_DIR}/optional"
    RUNTIME DESTINATION "${PLUGIN_DIR}/optional"
  )
  SET(CARLA_LIBRARIES ${CARLA_NATIVE_LIB})
  # Set parent scope variables so carlarack and carlapatchbay can see them
  SET(CARLA_LIBRARIES ${CARLA_LIBRARIES} PARENT_SCOPE)
endif()

if(LMMS_HAVE_CARLA OR LMMS_HAVE_WEAKCARLA)
        # Mimic the missing "config.h"
        SET(CARLA_INCLUDE_DIRS ${CARLA_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/CarlaConfig)
        SET(CARLA_INCLUDE_DIRS ${CARLA_INCLUDE_DIRS} PARENT_SCOPE)

        INCLUDE(BuildPlugin)
        include_directories(SYSTEM ${CARLA_INCLUDE_DIRS})
        # Probe declarations without linking new mandatory symbols. Runtime
        # resolution keeps instruments usable with older replacement libraries.
        include(CheckCXXSourceCompiles)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_INCLUDES ${CARLA_INCLUDE_DIRS})
        check_cxx_source_compiles("
            #include <CarlaNativePlugin.h>
            #include <type_traits>
            static_assert(std::is_pointer<decltype(&carla_create_native_plugin_host_handle)>::value, \"host handle\");
            static_assert(std::is_pointer<decltype(&carla_host_handle_free)>::value, \"host lifetime\");
            static_assert(std::is_pointer<decltype(&carla_switch_plugins)>::value, \"host ordering\");
            int main() { return static_cast<int>(CARLA_BACKEND_NAMESPACE::PLUGIN_CLAP); }
        " LMMS_CARLA_NATIVE_HOST_API)
        check_cxx_source_compiles("
            #include <CarlaUtils.h>
            #include <type_traits>
            static_assert(std::is_pointer<decltype(&carla_plugin_discovery_start)>::value, \"discovery startup\");
            static_assert(std::is_pointer<decltype(&carla_plugin_discovery_idle)>::value, \"discovery polling\");
            static_assert(std::is_pointer<decltype(&carla_plugin_discovery_stop)>::value, \"discovery cleanup\");
            int main() { return static_cast<int>(CARLA_BACKEND_NAMESPACE::PLUGIN_CLAP); }
        " LMMS_CARLA_NATIVE_DISCOVERY_API)
        check_cxx_source_compiles("
            #include <CarlaHost.h>
            #include <type_traits>
            using Callback = CARLA_BACKEND_NAMESPACE::EngineCallbackFunc;
            static_assert(std::is_same<decltype(&carla_set_engine_callback), void(*)(CarlaHostHandle, Callback, void*)>::value, \"graph callback\");
            static_assert(std::is_same<decltype(&carla_patchbay_refresh), bool(*)(CarlaHostHandle, bool)>::value, \"graph refresh\");
            static_assert(std::is_same<decltype(&carla_patchbay_connect), bool(*)(CarlaHostHandle, bool, unsigned int, unsigned int, unsigned int, unsigned int)>::value, \"graph connect\");
            static_assert(std::is_same<decltype(&carla_patchbay_disconnect), bool(*)(CarlaHostHandle, bool, unsigned int)>::value, \"graph disconnect\");
            int main() { return static_cast<int>(CARLA_BACKEND_NAMESPACE::PATCHBAY_PORT_TYPE_CV); }
        " LMMS_CARLA_NATIVE_PATCHBAY_API)
        cmake_pop_check_state()
        LINK_DIRECTORIES(${CARLA_LIBRARY_DIRS})
        LINK_LIBRARIES(${CARLA_LIBRARIES})
        BUILD_PLUGIN(carlabase Carla.cpp Carla.h CarlaPluginHost.cpp CarlaPluginHost.h
            CarlaPluginDiscovery.cpp CarlaPluginDiscovery.h
            CarlaPluginPatchbay.cpp CarlaPluginPatchbay.h
            MOCFILES Carla.h
            EMBEDDED_RESOURCES artwork-patchbay.png artwork-rack.png
            EXPORT_BASE_NAME carlabase
            LINK SHARED)
        if(LMMS_CARLA_NATIVE_HOST_API)
                target_compile_definitions(carlabase PUBLIC LMMS_CARLA_NATIVE_HOST_API)
        endif()
        if(LMMS_CARLA_NATIVE_DISCOVERY_API)
                target_compile_definitions(carlabase PUBLIC LMMS_CARLA_NATIVE_DISCOVERY_API)
        endif()
        if(LMMS_CARLA_NATIVE_PATCHBAY_API)
                target_compile_definitions(carlabase PUBLIC LMMS_CARLA_NATIVE_PATCHBAY_API)
        endif()
        SET_TARGET_PROPERTIES(carlabase
                PROPERTIES SKIP_BUILD_RPATH TRUE
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH_USE_LINK_PATH TRUE
                INSTALL_RPATH "${CARLA_RPATH}")
        IF(LMMS_HAVE_WEAKCARLA)
                ADD_DEPENDENCIES(carlabase ${CARLA_NATIVE_LIB})
        ENDIF()
endif()
