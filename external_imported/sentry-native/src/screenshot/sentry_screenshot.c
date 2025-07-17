#include "sentry_screenshot.h"

#include "sentry_database.h"

sentry_path_t *
sentry__screenshot_get_path(const sentry_options_t *options)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    return sentry__path_join_wstr(options->run->run_path, L"screenshot.png");
#else
    return sentry__path_join_str(options->run->run_path, "screenshot.png");
#endif
}
