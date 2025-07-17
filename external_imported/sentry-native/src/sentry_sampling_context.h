// sentry_sampling_context.h
#ifndef SENTRY_SAMPLING_CONTEXT_H_INCLUDED
#define SENTRY_SAMPLING_CONTEXT_H_INCLUDED

#include "sentry_tracing.h"

typedef struct sentry_sampling_context_s {
    sentry_transaction_context_t *transaction_context;
    sentry_value_t custom_sampling_context;
    bool *parent_sampled;
} sentry_sampling_context_t;

#endif // SENTRY_SAMPLING_CONTEXT_H_INCLUDED
