# as_players — Plugify C++ plugin: port of the Pisex admin_system module "Players".
#
#   plugify-as_players  the plugin shared library (entry "bin/as_players")
#
# Consumer of the admin_system API: compiles the generated binding globals of
# plugins/admin_system/include (the __admin_system_* slots filled by the module) and uses the
# hand-written admin_api.hpp wrapper plus pisexcompat. No database access of its own.

set(AS_PLAYERS_VERSION "1.0.1")

# The consumer side of the admin API (API.md §5: the generated .cpp is compiled into every
# consumer plugin, like s2sdk_bindings / permissions_bindings).
set(AS_PLAYERS_ADMIN_INCLUDE "${CMAKE_SOURCE_DIR}/plugins/admin_system/include")
set(AS_PLAYERS_ADMIN_BINDINGS
		"${AS_PLAYERS_ADMIN_INCLUDE}/external/plugify/include/admin_system/admin_system.cpp")

add_library(plugify-as_players SHARED
		src/plugin.cpp
		"${AS_PLAYERS_ADMIN_BINDINGS}"
)
target_include_directories(plugify-as_players PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated admin_system/plg/s2sdk headers must not fail -Werror on
# our sources (src/plugin.cpp stays warning-clean).
target_include_directories(plugify-as_players SYSTEM PRIVATE
		"${CMAKE_SOURCE_DIR}/sdk/include"
		"${AS_PLAYERS_ADMIN_INCLUDE}"
		"${AS_PLAYERS_ADMIN_INCLUDE}/external/plugify/include")
target_link_libraries(plugify-as_players PRIVATE
		pisexcompat # pulls in the s2sdk binding globals and the plg headers
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
