#include <signal.h>
#include <stdlib.h>
void native_crash(void)
{
    *(int *)10 = 100;
}

void enable_sigaltstack(void)
{
    const size_t signal_stack_size = 16384;
    stack_t signal_stack;
    signal_stack.ss_sp = malloc(signal_stack_size);
    if (!signal_stack.ss_sp) {
        return;
    }
    signal_stack.ss_size = signal_stack_size;
    signal_stack.ss_flags = 0;
    sigaltstack(&signal_stack, 0);
}