#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"
#include "qapi/qapi-types-misc-i386.h"
#include "hw/core/cpu.h"
#include "target/i386/cpu.h"
#include "cpu.h" 

void cpu_synchronize_state(CPUState *cpu);

X86RawDump *qmp_x86_dump_raw(Error **errp)
{
    CPUState *cs = current_cpu;
    if (!cs) {
        CPU_FOREACH(cs) { break; }
    }
    if (!cs) {
        error_setg(errp, "no CPU available");
        return NULL;
    }

    cpu_synchronize_state(cs);

    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    if (!mem) {
        error_setg_errno(errp, errno, "open_memstream failed");
        return NULL;
    }

    const int flags = CPU_DUMP_FPU;
    cpu_dump_state(cs, mem, flags);
    fclose(mem);

    X86RawDump *ret = g_new0(X86RawDump, 1);
    ret->text = g_strdup(buf);
    free(buf);
    return ret;
}
