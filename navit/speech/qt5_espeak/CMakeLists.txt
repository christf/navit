# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
# Instruct CMake to run moc automatically when needed.
set(CMAKE_AUTOMOC ON)

module_add_library(speech_qt5_espeak qt5_espeak.cpp Qt5EspeakAudioOut.cpp)
if(INTERNAL_ESPEAK_COMPLETE)
	target_compile_definitions(speech_qt5_espeak PRIVATE INTERNAL_ESPEAK=1)
	target_link_libraries(speech_qt5_espeak PRIVATE support_espeak)
	target_compile_options(speech_qt5_espeak PRIVATE -isystem ${CMAKE_SOURCE_DIR}/navit/support)
else()
	target_compile_definitions(speech_qt5_espeak PRIVATE INTERNAL_ESPEAK=0)
	if(espeak_FOUND)
		target_include_directories(speech_qt5_espeak PRIVATE ${espeak_INCLUDE_DIRS})
	endif()
endif()
