module_add_library(graphics_gd graphics_gd.c)
target_include_directories(graphics_gd PRIVATE ${GD_INCLUDE_DIR})
target_link_libraries(graphics_gd PRIVATE ${GD_LIBS})
