add_executable(gentypes gentypes.c)
target_link_libraries(gentypes PRIVATE navit_config navit_compile_options)

# add the command to generate the source code
add_custom_command (
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/g2nbuiltin.h
	COMMAND gentypes ${CMAKE_CURRENT_SOURCE_DIR}/garmintypes.txt ${CMAKE_CURRENT_BINARY_DIR}/g2nbuiltin.h
	DEPENDS gentypes
	)

module_add_library(map_garmin garmin.c gar2navit.c g2nbuiltin.h)
target_include_directories(map_garmin PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
