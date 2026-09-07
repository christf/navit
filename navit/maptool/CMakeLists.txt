if(BUILD_MAPTOOL)

	find_package(PROTOBUF_C REQUIRED)

	add_executable (maptool maptool.c)
	add_library (maptool_core boundaries.c buffer.c ch.c coastline.c itembin.c
		itembin_buffer.c itembin_slicer.c misc.c osm.c osm_o5m.c osm_psql.c
		osm_relations.c sourcesink.c tempfile.c tile.c zip.c osm_xml.c)

	target_compile_definitions(maptool PRIVATE MODULE=maptool)
	target_compile_definitions(maptool_core PRIVATE MODULE=maptool)
	target_link_libraries(maptool PRIVATE navit_config navit_compile_options navit_deps)
	target_link_libraries(maptool_core PRIVATE navit_config navit_compile_options navit_deps)

	if(NOT MSVC)
		PROTOBUF_C_GENERATE_C (PROTO_SRCS PROTO_HDRS osmformat.proto)
		PROTOBUF_C_GENERATE_C (PROTO_SRCS PROTO_HDRS fileformat.proto)
		target_sources(maptool_core PRIVATE osm_protobuf.c osm_protobufdb.c
			fileformat.pb-c.c osmformat.pb-c.c)
		target_include_directories(maptool_core PRIVATE ${PROTOBUF-C_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
		target_link_libraries(maptool_core PRIVATE ${PROTOBUF_C_LIBRARY})
	endif(NOT MSVC)

	target_link_libraries(maptool PRIVATE maptool_core ${NAVIT_LIBNAME} navit_deps)

	install(TARGETS maptool
		DESTINATION ${BIN_DIR}
		PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

endif()
