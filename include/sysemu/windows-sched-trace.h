/*
 * Windows scheduler tracing glue.
 *
 * Copyright (c) 2024
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef QEMU_SYSEMU_WINDOWS_SCHED_TRACE_H
#define QEMU_SYSEMU_WINDOWS_SCHED_TRACE_H

#include "exec/cpu-common.h"

/*
 * Accelerator backends call this hook when a vCPU returns from the
 * hypervisor.  The implementation is a no-op unless Windows scheduler
 * tracing has been enabled via QMP.  It also degrades gracefully on
 * targets where the feature is unsupported.
 */
void windows_sched_trace_post_run(CPUState *cpu);

#endif /* QEMU_SYSEMU_WINDOWS_SCHED_TRACE_H */
