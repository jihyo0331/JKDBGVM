#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"
#include "qapi/qapi-types-misc-i386.h"

X86RawDump *qmp_x86_dump_raw(Error **errp)
{
    error_setg(errp, "x86 raw dump is not available on this target");
    return NULL;
}

X86Regs *qmp_x86_info_register(Error **errp)
{
    error_setg(errp, "x86 register info is not available on this target");
    return NULL;
}

void qmp_x_tcg_set_icount_shift(int64_t value, Error **errp)
{
    (void)value;
    error_setg(errp, "icount shift control is not available on this target");
}
