sentry_target_sources_cwd(sentry
	sentry_alloc.c
	sentry_alloc.h
	sentry_app_hang_latch.c
	sentry_app_hang_latch.h
	sentry_app_hang_monitor.c
	sentry_app_hang_monitor.h
	sentry_attachment.c
	sentry_attachment.h
	sentry_backend.c
	sentry_backend.h
	sentry_batcher.c
	sentry_batcher.h
	sentry_boot.h
	sentry_client_report.c
	sentry_client_report.h
	sentry_core.c
	sentry_core.h
	sentry_cpu_relax.h
	sentry_database.c
	sentry_database.h
	sentry_envelope.c
	sentry_envelope.h
	sentry_hint.c
	sentry_hint.h
	sentry_info.c
	sentry_json.c
	sentry_json.h
	sentry_logger.c
	sentry_logger.h
	sentry_logs.c
	sentry_logs.h
	sentry_metrics.c
	sentry_metrics.h
	sentry_options.c
	sentry_options.h
	sentry_os.c
	sentry_os.h
	sentry_path.h
	sentry_process.h
	sentry_ratelimiter.c
	sentry_ratelimiter.h
	sentry_retry.c
	sentry_retry.h
	sentry_ringbuffer.c
	sentry_ringbuffer.h
	sentry_sampling_context.h
	sentry_scope.c
	sentry_scope.h
	sentry_screenshot.h
	sentry_session_replay.h
	sentry_session.c
	sentry_session.h
	sentry_slice.c
	sentry_slice.h
	sentry_string.c
	sentry_string.h
	sentry_symbolizer.h
	sentry_sync.c
	sentry_sync.h
	sentry_thread_stackwalk.h
	sentry_transport.c
	sentry_transport.h
	sentry_utils.c
	sentry_utils.h
	sentry_uuid.c
	sentry_uuid.h
	sentry_value.c
	sentry_value.h
	sentry_writer.c
	sentry_writer.h
	sentry_tracing.c
	sentry_tracing.h
	path/sentry_path.c
	screenshot/sentry_screenshot.c
	session_replay/sentry_session_replay.c
	transports/sentry_disk_transport.c
	transports/sentry_disk_transport.h
	transports/sentry_function_transport.c
	transports/sentry_http_transport.c
	transports/sentry_http_transport.h
	unwinder/sentry_unwinder.c
)

# generic platform / path / symbolizer
if(WIN32)
	sentry_target_sources_cwd(sentry
		sentry_random.c
		sentry_random.h
		sentry_windows_dbghelp.c
		sentry_windows_dbghelp.h
		path/sentry_path_windows.c
		process/sentry_process_windows.c
		symbolizer/sentry_symbolizer_windows.c
	)
elseif(NX OR PROSPERO)
	sentry_target_sources_cwd(sentry
		sentry_unix_spinlock.h
		path/sentry_path_unix.c
		process/sentry_process_none.c
	)
else()
	sentry_target_sources_cwd(sentry
		sentry_random.c
		sentry_random.h
		sentry_unix_pageallocator.c
		sentry_unix_pageallocator.h
		sentry_unix_spinlock.h
		symbolizer/sentry_symbolizer_unix.c
		path/sentry_path_unix.c
		process/sentry_process_unix.c
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
elseif(AIX)
	sentry_target_sources_cwd(sentry
		modulefinder/sentry_modulefinder_aix.c
	)
endif()

# transport
if(SENTRY_TRANSPORT_CURL)
	sentry_target_sources_cwd(sentry
		transports/sentry_http_transport_curl.c
	)
elseif(SENTRY_TRANSPORT_WINHTTP)
	sentry_target_sources_cwd(sentry
		transports/sentry_http_transport_winhttp.c
	)
elseif(SENTRY_TRANSPORT_NONE)
	sentry_target_sources_cwd(sentry
		transports/sentry_transport_none.c
	)
endif()

# backends
if(SENTRY_BACKEND_CRASHPAD)
	target_compile_definitions(sentry PRIVATE SENTRY_BACKEND_CRASHPAD)
	if(MSVC)
		target_compile_options(sentry PRIVATE
			$<$<COMPILE_LANGUAGE:C,CXX>:/wd4127> # Conditional expression is constant.
		)
	endif()
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
elseif(SENTRY_BACKEND_NATIVE)
	target_compile_definitions(sentry PRIVATE SENTRY_BACKEND_NATIVE)
	sentry_target_sources_cwd(sentry
		backends/sentry_backend_native.c
		backends/native/sentry_crash_ipc.c
		backends/native/sentry_crash_daemon.c
		backends/native/sentry_crash_handler.c
		backends/native/minidump/sentry_minidump_format.h
		backends/native/minidump/sentry_minidump_writer.h
	)

	# Remote unwinding (Linux only — uses libunwind ptrace accessors)
	if(LINUX)
		sentry_target_sources_cwd(sentry
			unwinder/sentry_unwinder_libunwind_remote.c
		)
	endif()

	# Platform-specific minidump writers
	if(LINUX OR ANDROID)
		sentry_target_sources_cwd(sentry
			backends/native/minidump/sentry_minidump_common.c
			backends/native/minidump/sentry_minidump_indirect.c
			backends/native/minidump/sentry_minidump_linux.c
		)
	elseif(APPLE)
		sentry_target_sources_cwd(sentry
			backends/native/minidump/sentry_minidump_common.c
			backends/native/minidump/sentry_minidump_indirect.c
			backends/native/minidump/sentry_minidump_macos.c
		)
	elseif(WIN32)
		sentry_target_sources_cwd(sentry
			backends/native/minidump/sentry_minidump_windows.c
		)
	endif()

	# Add include directory for native backend headers
	target_include_directories(sentry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/backends/native)

	# Platform-specific libraries for native backend
	if(LINUX OR ANDROID)
		# Linux needs pthread and rt for shared memory
		target_link_libraries(sentry PRIVATE pthread rt)
	elseif(APPLE)
		# macOS needs CoreFoundation and Security frameworks
		find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
		find_library(SECURITY_LIBRARY Security REQUIRED)
		target_link_libraries(sentry PRIVATE
			${COREFOUNDATION_LIBRARY}
			${SECURITY_LIBRARY}
		)
	elseif(WIN32)
		if(NOT XBOX)
			# Windows needs dbghelp for MiniDumpWriteDump. On Xbox console
			# MiniDumpWriteDump is supplied by xgameplatform.lib (linked by
			# the Xbox toolchain as a standard library), so skip dbghelp.
			target_link_libraries(sentry PRIVATE dbghelp)
		endif()
	endif()

	# Enable C11 for atomics support
	set_property(TARGET sentry PROPERTY C_STANDARD 11)
elseif(SENTRY_BACKEND_NONE)
	sentry_target_sources_cwd(sentry
		backends/sentry_backend_none.c
	)
endif()

# unwinder
sentry_target_sources_cwd(sentry
	unwinder/sentry_unwinder.h
)
if(SENTRY_WITH_LIBBACKTRACE)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_LIBBACKTRACE)
	sentry_target_sources_cwd(sentry
		unwinder/sentry_unwinder_libbacktrace.c
	)
endif()

if(SENTRY_WITH_LIBUNWIND)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_LIBUNWIND)
	sentry_target_sources_cwd(sentry
		unwinder/sentry_unwinder_libunwind.c
	)
endif()

if(SENTRY_WITH_LIBUNWIND_MAC)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_LIBUNWIND_MAC)
	sentry_target_sources_cwd(sentry
			unwinder/sentry_unwinder_libunwind_mac.c
	)
endif()

# platform thread stackwalker (suspend a thread and capture its backtrace)
if(APPLE)
	sentry_target_sources_cwd(sentry
		sentry_thread_stackwalk_mach.c
	)
endif()

if(WIN32)
	sentry_target_sources_cwd(sentry
		sentry_thread_stackwalk_windows.c
	)
endif()

if(LINUX OR ANDROID)
	sentry_target_sources_cwd(sentry
		sentry_thread_stackwalk_posix.c
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

if(PROSPERO)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_UNWINDER_PS)
endif()

# integrations
if(SENTRY_INTEGRATION_QT)
	target_compile_definitions(sentry PRIVATE SENTRY_INTEGRATION_QT)
	sentry_target_sources_cwd(sentry
		integrations/sentry_integration_qt.cpp
		integrations/sentry_integration_qt.h
	)
endif()

# screenshot
if(SENTRY_SCREENSHOT_WINDOWS)
	target_compile_definitions(sentry PRIVATE SENTRY_SCREENSHOT_WINDOWS)
	sentry_target_sources_cwd(sentry
		screenshot/sentry_screenshot_windows.c
	)
elseif(SENTRY_SCREENSHOT_NONE)
	target_compile_definitions(sentry PRIVATE SENTRY_SCREENSHOT_NONE)
	sentry_target_sources_cwd(sentry
		screenshot/sentry_screenshot_none.c
	)
elseif(SENTRY_SCREENSHOT_CUSTOM)
	target_compile_definitions(sentry PRIVATE SENTRY_SCREENSHOT_CUSTOM)
endif()

# session replay
if(NOT XBOX)
	sentry_target_sources_cwd(sentry
		session_replay/sentry_session_replay_none.c
	)
endif()
