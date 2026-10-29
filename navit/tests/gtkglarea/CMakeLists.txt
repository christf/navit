#####################################################################################
# Tests for the graphics/gtkglarea backend.
#
# These drive the real plugin through the public graphics API and read the framebuffer
# back, so they need a working GL context. They are only meaningful on a machine with a
# GPU or a software GL stack, and are therefore not part of the default test run.
#
# The harness dlopens graphics/gtkglarea/libgraphics_gtkglarea.so by path, so it belongs to a
# plugin build. The static tree used by scripts/run_core_tests.sh is not applicable here.
#####################################################################################

if(NOT USE_PLUGINS)
    message(STATUS "gtkglarea tests need a plugin build, skipping")
    return()
endif()

if(NOT graphics/gtkglarea)
    return()
endif()

find_program(XVFB_RUN_EXECUTABLE xvfb-run)
if(NOT XVFB_RUN_EXECUTABLE)
    message(STATUS "xvfb-run not found, skipping gtkglarea tests")
    return()
endif()

add_executable(navit_gtkglarea_test test_gtkglarea_linewidth.c)
target_link_libraries(navit_gtkglarea_test PRIVATE
    ${NAVIT_LIBNAME}
    navit_config
    navit_compile_options
    PkgConfig::GTK4 PkgConfig::EPoxy PkgConfig::GMODULE)

add_test(NAME gtkglarea_linewidth
    COMMAND ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 1024x768x24"
            $<TARGET_FILE:navit_gtkglarea_test> ${CMAKE_BINARY_DIR}/navit)
set_tests_properties(gtkglarea_linewidth PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
