#ifndef SENTRY_SCREENSHOT_H_INCLUDED
#define SENTRY_SCREENSHOT_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_options.h"
#include "sentry_path.h"

/**
 * Captures a screenshot and saves it to the specified path.
 *
 * Returns true if the screenshot was successfully captured and saved.
 */
bool sentry__screenshot_capture(const sentry_path_t *path);

/**
 * Returns the path where a screenshot should be saved.
 */
sentry_path_t *sentry__screenshot_get_path(const sentry_options_t *options);

#endif
