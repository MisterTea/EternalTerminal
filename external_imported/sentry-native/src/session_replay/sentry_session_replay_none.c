#include "sentry_session_replay.h"

#include "sentry_core.h"

bool
sentry__session_replay_capture(const sentry_path_t *UNUSED(path),
    uint32_t UNUSED(duration_ms), uint32_t UNUSED(pid))
{
    return false;
}
