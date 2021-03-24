function(sentry_get_property NAME)
	get_target_property(prop sentry "${NAME}")
	if(NOT prop)
		set(prop)
	endif()
	set("SENTRY_${NAME}" "${prop}" PARENT_SCOPE)
endfunction()

sentry_get_property(SOURCES)
sentry_get_property(COMPILE_DEFINITIONS)
sentry_get_property(INTERFACE_INCLUDE_DIRECTORIES)
sentry_get_property(INCLUDE_DIRECTORIES)
sentry_get_property(LINK_LIBRARIES)
sentry_get_property(INTERFACE_LINK_LIBRARIES)

add_executable(sentry_test_unit
	${SENTRY_SOURCES}
	main.c
	sentry_testsupport.h
	test_attachments.c
	test_basic.c
	test_consent.c
	test_envelopes.c
	test_failures.c
	test_logger.c
	test_modulefinder.c
	test_mpack.c
	test_path.c
	test_ratelimiter.c
	test_session.c
	test_slice.c
	test_symbolizer.c
	test_sync.c
	test_uninit.c
	test_unwinder.c
	test_utils.c
	test_uuid.c
	test_value.c
	tests.inc
)

# FIXME: cmake 3.13 introduced target_link_options
target_compile_definitions(sentry_test_unit PRIVATE ${SENTRY_COMPILE_DEFINITIONS})
target_include_directories(sentry_test_unit PRIVATE
	${SENTRY_INTERFACE_INCLUDE_DIRECTORIES}
	${SENTRY_INCLUDE_DIRECTORIES}
)
target_link_libraries(sentry_test_unit PRIVATE
	${SENTRY_LINK_LIBRARIES}
	${SENTRY_INTERFACE_LINK_LIBRARIES}
	"$<$<PLATFORM_ID:Linux>:rt>"
)

if(MINGW)
	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		target_link_options(sentry_test_unit PRIVATE -Wl,-pdb=) # IMPORTANT ! LLD must generate pdb files
		target_compile_options(sentry_test_unit PRIVATE -gcodeview) # IMPORTANT ! Clang must generate pdb to CodeView format
	else()
		message(FATAL_ERROR "Tests will not pass on MinGW if your compiler cannot generate .pdb files ! Please use Clang instead")
	endif()
	target_compile_options(sentry_test_unit PRIVATE
		-Wno-unused-variable
		-Wno-unused-parameter
		-Wno-format
		-Wno-incompatible-pointer-types
		-Wno-incompatible-function-pointer-types
	)
endif()

if(MSVC)
	target_compile_options(sentry_test_unit PRIVATE $<BUILD_INTERFACE:/wd5105>)
endif()

# set static runtime if enabled
if(SENTRY_BUILD_RUNTIMESTATIC AND MSVC)
	set_property(TARGET sentry_test_unit PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_compile_definitions(sentry PRIVATE SIZEOF_LONG=${CMAKE_SIZEOF_LONG})

target_compile_definitions(sentry_test_unit PRIVATE SENTRY_UNITTEST)

add_test(NAME sentry_test_unit COMMAND sentry_test_unit)
