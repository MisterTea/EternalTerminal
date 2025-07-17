# Generates a version resource file from the `sentry.rc.in` template for the `TGT` argument and adds it as a source.
function(sentry_add_version_resource TGT FILE_DESCRIPTION)
	# generate a resource output-path from the target name
	set(RESOURCE_PATH "${CMAKE_CURRENT_BINARY_DIR}/${TGT}.rc")
	set(RESOURCE_PATH_TMP "${RESOURCE_PATH}.in")

	# Extract major, minor and patch version from SENTRY_VERSION
	string(REPLACE "." ";" _SENTRY_VERSION_LIST "${SENTRY_VERSION}")
	list(GET _SENTRY_VERSION_LIST 0 SENTRY_VERSION_MAJOR)
	list(GET _SENTRY_VERSION_LIST 1 SENTRY_VERSION_MINOR)
	list(GET _SENTRY_VERSION_LIST 2 SENTRY_VERSION_PATCH)

	# Produce the resource file with configure-time replacements
	configure_file("${SENTRY_SOURCE_DIR}/sentry.rc.in" "${RESOURCE_PATH_TMP}" @ONLY)

	# Replace the `ORIGINAL_FILENAME` at generate-time using the generator expression `TARGET_FILE_NAME`
	file(GENERATE OUTPUT ${RESOURCE_PATH} INPUT ${RESOURCE_PATH_TMP})

	# Finally add the generated resource file to the target sources
	target_sources("${TGT}" PRIVATE "${RESOURCE_PATH}")
endfunction()
