#ifndef SENTRY_INTEGRATION_QT_H_INCLUDED
#define SENTRY_INTEGRATION_QT_H_INCLUDED

#ifdef __cplusplus
#    define C_API extern "C"
#else
#    define C_API
#endif

/**
 * This sets up the Qt integration.
 */
C_API void sentry_integration_setup_qt(void);

#endif
