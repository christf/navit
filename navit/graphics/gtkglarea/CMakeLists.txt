set(graphics_gtkglarea_LIBS ${GTK4_LIBRARIES})
module_add_library(graphics_gtkglarea graphics_gtkglarea.c)
target_include_directories(graphics_gtkglarea PRIVATE ${GTK4_INCLUDE_DIRS})
target_link_directories(graphics_gtkglarea PRIVATE ${GTK4_LIBRARY_DIRS})
