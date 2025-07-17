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

list(FILTER SENTRY_SOURCES EXCLUDE REGEX "\\.rc$")

add_executable(sentry_benchmark
	${SENTRY_SOURCES}
	benchmark_init.cpp
	benchmark_backend.cpp
)

if(SENTRY_BACKEND_CRASHPAD)
	add_dependencies(sentry_benchmark crashpad::handler)
	if(WIN32)
		add_dependencies(sentry_benchmark crashpad::wer)
	endif()
endif()

target_link_libraries(sentry_benchmark PRIVATE
	${SENTRY_LINK_LIBRARIES}
	${SENTRY_INTERFACE_LINK_LIBRARIES}
	"$<$<PLATFORM_ID:Linux>:rt>"
	benchmark::benchmark benchmark::benchmark_main
)

target_compile_definitions(sentry_benchmark PRIVATE ${SENTRY_COMPILE_DEFINITIONS})
target_include_directories(sentry_benchmark PRIVATE
	${SENTRY_INTERFACE_INCLUDE_DIRECTORIES}
	${SENTRY_INCLUDE_DIRECTORIES}
)

if(MSVC)
	target_compile_options(sentry_benchmark PRIVATE $<BUILD_INTERFACE:/wd5105>)
endif()

# set static runtime if enabled
if(SENTRY_BUILD_RUNTIMESTATIC AND MSVC)
	set_property(TARGET sentry_benchmark PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME sentry_benchmark COMMAND sentry_benchmark)
