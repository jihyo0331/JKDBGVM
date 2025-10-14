#include "qemu/osdep.h"
#include "disas/disas.h"

const char * __attribute__((weak)) lookup_symbol(uint64_t addr)
{
    (void)addr;
    return NULL;
}
