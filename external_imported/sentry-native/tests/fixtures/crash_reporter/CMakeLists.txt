cmake_minimum_required(VERSION 3.10)
project(sentry_crash_reporter LANGUAGES C)

add_executable(sentry_crash_reporter crash_reporter.c)
target_link_libraries(sentry_crash_reporter PRIVATE sentry)
