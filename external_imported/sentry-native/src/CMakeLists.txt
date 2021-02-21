sentry_target_sources_cwd(sentry
	sentry_alloc.c
	sentry_alloc.h
	sentry_backend.c
	sentry_backend.h
	sentry_boot.h
	sentry_core.c
	sentry_core.h
	sentry_database.c
	sentry_database.h
	sentry_envelope.c
	sentry_envelope.h
	sentry_json.c
	sentry_json.h
	sentry_logger.c
	sentry_logger.h
	sentry_options.c
	sentry_options.h
	sentry_os.c
	sentry_os.h
	sentry_path.h
	sentry_random.c
	sentry_random.h
	sentry_ratelimiter.c
	sentry_ratelimiter.h
	sentry_scope.c
	sentry_scope.h
	sentry_session.c
	sentry_session.h
	sentry_slice.c
	sentry_slice.h
	sentry_string.c
	sentry_string.h
	sentry_symbolizer.h
	sentry_sync.c
	sentry_sync.h
	sentry_transport.c
	sentry_transport.h
	sentry_utils.c
	sentry_utils.h
	sentry_uuid.c
	sentry_uuid.h
	sentry_value.c
	sentry_value.h
	path/sentry_path.c
	transports/sentry_disk_transport.c
	transports/sentry_disk_transport.h
	transports/sentry_function_transport.c
	unwinder/sentry_unwinder.c
)

# generic platform / path / symbolizer
if(WIN32)
	sentry_target_sources_cwd(sentry
		sentry_windows_dbghelp.c
		sentry_windows_dbghelp.h
		path/sentry_path_windows.c
		symbolizer/sentry_symbolizer_windows.c
	)
else()
	sentry_target_sources_cwd(sentry
		sentry_unix_pageallocator.c
		sentry_unix_pageallocator.h
		sentry_unix_spinlock.h
		path/sentry_path_unix.c
		symbolizer/sentry_symbolizer_unix.c
	)
endif()

# module finder
if(WIN32)
	sentry_target_sources_cwd(sentry
		modulefinder/sentry_modulefinder_windows.c
	)
elseif(APPLE)
	sentry_target_sources_cwd(sentry
		modulefinder/sentry_modulefinder_apple.c
	)
elseif(LINUX OR ANDROID)
	sentry_target_sources_cwd(sentry
		modulefinder/sentry_modulefinder_linux.c
	)
endif()

# transport
if(SENTRY_TRANSPORT_CURL)
	sentry_target_sources_cwd(sentry
		transports/sentry_transport_curl.c
	)
elseif(SENTRY_TRANSPORT_WINHTTP)
	sentry_target_sources_cwd(sentry
		transports/sentry_transport_winhttp.c
	)
elseif(SENTRY_TRANSPORT_NONE)
	sentry_target_sources_cwd(sentry
		transports/sentry_transport_none.c
	)
endif()

# backends
if(SENTRY_BACKEND_CRASHPAD)
    target_compile_definitions(sentry PRIVATE SENTRY_BACKEND_CRASHPAD)
    sentry_target_sources_cwd(sentry
        backends/sentry_backend_crashpad.cpp
    )
elseif(SENTRY_BACKEND_BREAKPAD)
    target_compile_definitions(sentry PRIVATE SENTRY_BACKEND_BREAKPAD)
    sentry_target_sources_cwd(sentry
        backends/sentry_backend_breakpad.cpp
    )
elseif(SENTRY_BACKEND_INPROC)
    target_compile_definitions(sentry PRIVATE SENTRY_BACKEND_INPROC)
    sentry_target_sources_cwd(sentry
        backends/sentry_backend_inproc.c
    )
elseif(SENTRY_BACKEND_NONE)
    sentry_target_sources_cwd(sentry
        backends/sentry_backend_none.c
    )
endif()

# unwinder
if(SENTRY_WITH_LIBBACKTRACE)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_LIBBACKTRACE)
	sentry_target_sources_cwd(sentry
		unwinder/sentry_unwinder_libbacktrace.c
	)
endif()
if(SENTRY_WITH_LIBUNWINDSTACK)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
	sentry_target_sources_cwd(sentry
		unwinder/sentry_unwinder_libunwindstack.cpp
	)
endif()
if(WIN32)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_DBGHELP)
	sentry_target_sources_cwd(sentry
		unwinder/sentry_unwinder_dbghelp.c
	)
endif()

# integrations
if(SENTRY_INTEGRATION_QT)
	target_compile_definitions(sentry PRIVATE SENTRY_INTEGRATION_QT)
	sentry_target_sources_cwd(sentry
		integrations/sentry_integration_qt.cpp
		integrations/sentry_integration_qt.h
	)
endif()
