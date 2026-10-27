# Generates a version resource file from the `sentry.rc.in` template for the `TGT` argument and adds it as a source.
function(sentry_add_version_resource TGT FILE_DESCRIPTION)
	# generate a multi-config aware resource output-path from the target name
	set(RESOURCE_BASENAME "${TGT}.rc")
	set(RESOURCE_PATH_TMP "${CMAKE_CURRENT_BINARY_DIR}/${RESOURCE_BASENAME}.in")
	set(RESOURCE_PATH "${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>/,>${RESOURCE_BASENAME}")

	# Produce the resource file with configure-time replacements
	configure_file("${SENTRY_SOURCE_DIR}/sentry.rc.in" "${RESOURCE_PATH_TMP}" @ONLY)

	# Replace the `ORIGINAL_FILENAME` at generate-time using the generator expression `TARGET_FILE_NAME`
	file(GENERATE OUTPUT ${RESOURCE_PATH} INPUT ${RESOURCE_PATH_TMP})

	# Finally add the generated resource file to the target sources
	target_sources("${TGT}" PRIVATE "${RESOURCE_PATH}")
endfunction()
