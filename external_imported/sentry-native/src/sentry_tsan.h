#ifndef SENTRY_TSAN_H_INCLUDED
#define SENTRY_TSAN_H_INCLUDED

// Provide safe access to the thread sanitizer interface

#ifdef __has_feature
#    if __has_feature(thread_sanitizer)
#        ifdef __cplusplus
extern "C" {
#        endif
void AnnotateIgnoreReadsBegin(const char *file, int line);
void AnnotateIgnoreReadsEnd(const char *file, int line);
#        ifdef __cplusplus
}
#        endif
#        define SENTRY_TSAN_IGNORE_READS_BEGIN()                               \
            AnnotateIgnoreReadsBegin(__FILE__, __LINE__)
#        define SENTRY_TSAN_IGNORE_READS_END()                                 \
            AnnotateIgnoreReadsEnd(__FILE__, __LINE__)
#    else
#        define SENTRY_TSAN_IGNORE_READS_BEGIN()
#        define SENTRY_TSAN_IGNORE_READS_END()
#    endif
#else
#    define SENTRY_TSAN_IGNORE_READS_BEGIN()
#    define SENTRY_TSAN_IGNORE_READS_END()
#endif

#endif // SENTRY_TSAN_H_INCLUDED
