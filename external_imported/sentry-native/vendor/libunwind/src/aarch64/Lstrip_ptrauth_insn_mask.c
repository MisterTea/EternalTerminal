#define UNW_LOCAL_ONLY
#include <libunwind.h>
#if defined(UNW_LOCAL_ONLY) && !defined(UNW_REMOTE_ONLY)
#include "Gstrip_ptrauth_insn_mask.c"
#endif