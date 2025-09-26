/*
 * Stub implementation for Windows scheduler tracing on non-x86 targets.
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "qapi/qapi-commands-machine.h"
#include "sysemu/windows-sched-trace.h"

__attribute__((weak)) void windows_sched_trace_post_run(CPUState *cpu)
{
    (void)cpu;
}

__attribute__((weak)) WindowsSchedTraceEntryList *qmp_query_windows_sched_trace(
        bool has_max_entries, uint16_t max_entries,
        bool has_filter_vcpu, uint16_t filter_vcpu,
        bool has_filter_pid, uint64_t filter_pid,
        bool has_filter_tid, uint64_t filter_tid,
        Error **errp)
{
    (void)has_max_entries;
    (void)max_entries;
    (void)has_filter_vcpu;
    (void)filter_vcpu;
    (void)has_filter_pid;
    (void)filter_pid;
    (void)has_filter_tid;
    (void)filter_tid;

    error_setg(errp, "Windows scheduler tracing is only available on x86_64 targets");
    return NULL;
}

__attribute__((weak)) void qmp_windows_sched_trace_set(bool enable,
                                 bool has_auto_detect, bool auto_detect,
                                 WindowsSchedTraceOverrides *overrides,
                                 Error **errp)
{
    (void)has_auto_detect;
    (void)auto_detect;

    if (enable) {
        error_setg(errp, "Windows scheduler tracing is only available on x86_64 targets");
    }

    if (overrides) {
        qapi_free_WindowsSchedTraceOverrides(overrides);
    }
}
