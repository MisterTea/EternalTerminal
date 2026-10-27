#ifndef SENTRY_CPU_RELAX_H_INCLUDED
#define SENTRY_CPU_RELAX_H_INCLUDED

#if defined(_MSC_VER)
#    include <intrin.h>
#    if defined(_M_IX86) || defined(_M_X64)
#        define sentry__cpu_relax() _mm_pause()
#    elif defined(_M_ARM) || defined(_M_ARM64)
#        define sentry__cpu_relax() __yield()
#    else
#        define sentry__cpu_relax() __nop()
#    endif
#else
#    if defined(__i386__) || defined(__x86_64__) || defined(__amd64__)
#        define sentry__cpu_relax() __asm__ __volatile__("pause")
#    elif defined(__aarch64__) || defined(__arm__)
#        define sentry__cpu_relax() __asm__ __volatile__("yield")
#    elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
#        define sentry__cpu_relax() __asm__ __volatile__("or 27,27,27")
#    elif defined(__riscv)
#        if defined(__riscv_zihintpause)
#            define sentry__cpu_relax() __asm__ __volatile__("pause")
#        else
#            define sentry__cpu_relax() __asm__ __volatile__("nop")
#        endif
#    else
#        define sentry__cpu_relax() (void)0
#    endif
#endif

#endif // SENTRY_CPU_RELAX_H_INCLUDED
