set(graphics_gtk_drawing_area_LIBS ${IMLIB2_LDFLAGS} ${GTK4_LIBRARIES})
module_add_library(graphics_gtk_drawing_area graphics_gtk_drawing_area.c)
target_include_directories(graphics_gtk_drawing_area PRIVATE ${GTK4_INCLUDE_DIRS})
target_link_directories(graphics_gtk_drawing_area PRIVATE ${GTK4_LIBRARY_DIRS})
if(WIN32 AND NOT MSVC)
	target_compile_options(graphics_gtk_drawing_area PRIVATE "-mms-bitfields")
endif()

