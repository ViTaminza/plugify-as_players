# as_players — Plugify C++ plugin: port of the Pisex admin_system module "Players".
#
#   plugify-as_players  the plugin shared library (entry "bin/as_players")
#
# Pure s2sdk, no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7). The guards of
# handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself: the B1/B2 map gate, the C2
# client/slot/handle checks, the C10 menu lifecycle and the C11 command ownership rules. The
# plugin has no config, translation or database file of its own; phrases come from the
# admin_system API.
#
# Consumer of the admin_system API: compiles the generated binding globals of
# plugins/admin_system/include (the __admin_system_* slots filled by the module) and the
# hand-written admin_api.hpp wrapper, plus the s2sdk binding globals (__s2sdk_*). No shared
# wrapper library is linked.

set(AS_PLAYERS_VERSION "1.0.4")

# The optional hudmenu plugin API (GetClientMenuType: the original's per-player "!menu" type).
# The root CMakeLists builds the same binding globals as hudmenu_bindings for pisexcompat; the
# fallback target is for a build of this directory alone and has a plugin-specific name.
if(TARGET hudmenu_bindings)
    set(AS_PLAYERS_HUDMENU_BINDING_OBJECTS $<TARGET_OBJECTS:hudmenu_bindings>)
else()
    add_library(as_players_hudmenu_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(as_players_hudmenu_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_players_hudmenu_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_PLAYERS_HUDMENU_BINDING_OBJECTS $<TARGET_OBJECTS:as_players_hudmenu_bindings_obj>)
endif()

# The generated admin_system binding globals (__admin_system_*) have to be compiled into every
# consumer plugin; the admin_system plugin directory already builds them as an object library
# (it is added before plugins/as_players), this is the fallback for a build of this directory
# alone. The fallback target has a plugin-specific name so it can never collide with another
# plugin's target in a shared tree.
if(NOT TARGET admin_system_bindings_obj)
    add_library(as_players_admin_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(as_players_admin_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_players_admin_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_PLAYERS_ADMIN_BINDING_OBJECTS $<TARGET_OBJECTS:as_players_admin_bindings_obj>)
else()
    set(AS_PLAYERS_ADMIN_BINDING_OBJECTS $<TARGET_OBJECTS:admin_system_bindings_obj>)
endif()

add_library(plugify-as_players SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${AS_PLAYERS_HUDMENU_BINDING_OBJECTS}
        ${AS_PLAYERS_ADMIN_BINDING_OBJECTS}
)
target_include_directories(plugify-as_players PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated admin_system/plg/s2sdk headers and admin_api.hpp are not ours to fix;
# their warnings must not fail -Werror on our sources (src/plugin.cpp stays warning-clean).
target_include_directories(plugify-as_players SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_players PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_players PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_players PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_players"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_players PROPERTIES OUTPUT_NAME as_players)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_players PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_players.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_players.pplugin"
)

# --- tests ---------------------------------------------------------------------
# as_players_tests: engine-free. The plugin TU is compiled into the test (its handlers live in an
# anonymous namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake:
# the permission checks and command contexts, the player list, the map gate and slot reuse, the
# menu lifecycle, the missing-core/missing-binding paths and the unload rules. Nothing here
# links pisexcompat or a shared wrapper library. The hudmenu exports are optional: the tests
# install their own fakes even when the plugin is built with the real binding globals.
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_players_tests
            tests/as_players_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            ${AS_PLAYERS_HUDMENU_BINDING_OBJECTS}
            ${AS_PLAYERS_ADMIN_BINDING_OBJECTS}
    )
    target_include_directories(as_players_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_players_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(as_players_tests PRIVATE plugify_sdk -static-libstdc++ -static-libgcc)
    target_compile_options(as_players_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_players_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_players"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
    )
    set_target_properties(as_players_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
