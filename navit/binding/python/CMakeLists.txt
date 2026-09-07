module_add_library(binding_python attr.c binding_python.c config.c navigation.c navit.c pcoord.c route.c)
target_include_directories(binding_python PRIVATE ${Python2_INCLUDE_DIRS})
