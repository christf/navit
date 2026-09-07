supportlib_add_library(support_gettext_intl bindtextdom.c dcgettext.c dcigettext.c dcngettext.c dgettext.c dngettext.c explodename.c finddomain.c gettext.c intl-compat.c l10nflist.c loadmsgcat.c localcharset.c localealias.c localename.c log.c ngettext.c osdep.c plural-exp.c plural.c printf-args.c printf.c relocatable.c textdomain.c vasnprintf.c)
target_compile_definitions(support_gettext_intl PRIVATE
	LOCALE_ALIAS_PATH=LOCALEDIR
	IN_LIBINTL=1
	HAVE_CONFIG_H=1
	ENABLE_RELOCATABLE=1
	IN_LIBRARY
	NO_XMALLOC
	set_relocation_prefix=libintl_set_relocation_prefix
	relocate=libintl_relocate
	DEPENDS_ON_LIBICONV=1
	"LIBDIR=\"${CMAKE_INSTALL_PREFIX}/${LIB_DIR}\""
)
ADD_CUSTOM_TARGET(libintl.h DEPENDS libgnuintl.h COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/libgnuintl.h ${CMAKE_CURRENT_BINARY_DIR}/libintl.h)
ADD_DEPENDENCIES(support_gettext_intl libintl.h)
