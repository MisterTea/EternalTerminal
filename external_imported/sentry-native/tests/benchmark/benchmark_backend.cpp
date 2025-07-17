#include <benchmark/benchmark.h>

extern "C" {
#include "sentry_backend.h"
#include "sentry_database.h"
#include "sentry_logger.h"
#include "sentry_options.h"
#include "sentry_path.h"
}

static void
benchmark_backend_startup(benchmark::State &state)
{
    sentry_options_t *options = sentry_options_new();
    sentry_backend_t *backend = sentry__backend_new();

    if (!backend || !backend->startup_func) {
        state.SkipWithMessage("no backend/startup_func");
    } else {
        // support SENTRY_DEBUG=1
        sentry_logger_t logger = { NULL, NULL, SENTRY_LEVEL_DEBUG };
        if (options->debug) {
            logger = options->logger;
        }
        sentry__logger_set_global(logger);

        sentry__path_create_dir_all(options->database_path);
        sentry_path_t *database_path = options->database_path;
        options->database_path = sentry__path_absolute(database_path);
        sentry__path_free(database_path);
        options->run = sentry__run_new(options->database_path);

        for (auto s : state) {
            backend->startup_func(backend, options);
        }
    }

    sentry__backend_free(backend);
    sentry_options_free(options);
}

BENCHMARK(benchmark_backend_startup)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);
