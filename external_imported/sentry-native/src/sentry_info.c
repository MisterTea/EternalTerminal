#include "sentry_boot.h"

const char *
sentry_sdk_version(void)
{
    return SENTRY_SDK_VERSION;
}

const char *
sentry_sdk_name(void)
{
    return SENTRY_SDK_NAME;
}

const char *
sentry_sdk_user_agent(void)
{
    return SENTRY_SDK_USER_AGENT;
}
