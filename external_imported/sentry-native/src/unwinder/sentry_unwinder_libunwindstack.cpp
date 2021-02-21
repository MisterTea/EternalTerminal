extern "C" {
#include "sentry_boot.h"
}

#include <memory>
#include <ucontext.h>
#include <unwindstack/Elf.h>
#include <unwindstack/MapInfo.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsGetLocal.h>

extern "C" {

size_t
sentry__unwind_stack_libunwindstack(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    std::unique_ptr<unwindstack::Regs> regs;

    if (uctx) {
        regs = std::unique_ptr<unwindstack::Regs>(
            unwindstack::Regs::CreateFromUcontext(
                unwindstack::Regs::CurrentArch(), uctx->user_context));
    } else if (!addr) {
        regs = std::unique_ptr<unwindstack::Regs>(
            unwindstack::Regs::CreateFromLocal());
        unwindstack::RegsGetLocal(regs.get());
    } else {
        return 0;
    }

    unwindstack::LocalMaps maps;
    if (!maps.Parse()) {
        ptrs[0] = (void *)regs->pc();
        return 1;
    }

    const std::shared_ptr<unwindstack::Memory> process_memory(
        new unwindstack::MemoryLocal);

    int rv = 0;
    for (size_t i = 0; i < max_frames; i++) {
        ptrs[rv++] = (void *)regs->pc();
        unwindstack::MapInfo *map_info = maps.Find(regs->pc());
        if (!map_info) {
            break;
        }

        // the boolean false parameter disables debugdata loading which we don't
        // want due to size constraints.  Also that data is unlikely to be
        // useful anyways.
        unwindstack::Elf *elf = map_info->GetElf(process_memory, false);
        if (!elf) {
            break;
        }

        uint64_t rel_pc = elf->GetRelPc(regs->pc(), map_info);
        uint64_t adjusted_rel_pc = rel_pc - regs->GetPcAdjustment(rel_pc, elf);
        bool finished = false;
        if (!elf->Step(rel_pc, adjusted_rel_pc, map_info->elf_offset,
                regs.get(), process_memory.get(), &finished)) {
            break;
        }
    }

    return rv;
}
}
