if(NOT USE_PLUGINS)
    add_executable(navit_core_test core/navit_core_test.c)

    target_link_libraries(navit_core_test PRIVATE
        -Wl,--start-group ${NAVIT_LIBNAME} ${MODULES_NAME} ${NAVIT_SUPPORT_LIBS} navit_deps
        -Wl,--end-group)
    target_link_libraries(navit_core_test PRIVATE navit_config navit_compile_options)
    target_include_directories(navit_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vehicle/demo")

    set(FIXTURE_BIN "${CMAKE_CURRENT_BINARY_DIR}/fixture.bin")
    add_custom_command(
        OUTPUT ${FIXTURE_BIN}
        COMMAND $<TARGET_FILE:maptool> -i ${CMAKE_CURRENT_SOURCE_DIR}/core/fixture.osm ${FIXTURE_BIN}
        DEPENDS maptool ${CMAKE_CURRENT_SOURCE_DIR}/core/fixture.osm
        VERBATIM)
    add_custom_target(navit_core_test_fixture DEPENDS ${FIXTURE_BIN})
    add_dependencies(navit_core_test navit_core_test_fixture)

    add_test(NAME core COMMAND navit_core_test all)
    set_tests_properties(core PROPERTIES
        ENVIRONMENT "NAVIT_TEST_MAP=${FIXTURE_BIN}"
        TIMEOUT 600)
endif()

add_subdirectory(gtkglarea)
