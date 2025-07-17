#include <benchmark/benchmark.h>
#include <sentry.h>

static void
benchmark_init(benchmark::State &state)
{
    sentry_options_t *options = sentry_options_new();
    for (auto _ : state) {
        sentry_init(options);
    }
    sentry_close();
}

BENCHMARK(benchmark_init)->Iterations(1)->Unit(benchmark::kMillisecond);
