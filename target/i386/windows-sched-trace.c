/*
 * Windows scheduler tracing and QMP surface.
 *
 * This module samples the Windows kernel scheduler state for x86
 * guests and exposes the collected events through dedicated QMP
 * commands.  When tracing is disabled the hooks boil down to cheap
 * checks so that the accelerator hot path stays lean.
 *
 * The implementation relies on layout information published via the
 * guest's KDDEBUGGER_DATA64 block when auto-detection is enabled.  The
 * user may also supply manual overrides via QMP if the target build
 * deviates from the expected layout.
 *
 * Copyright (c) 2024
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-types-machine.h"

#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "hw/boards.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "sysemu/windows-sched-trace.h"
#include "target/i386/cpu.h"
#include "contrib/elf2dmp/kdbg.h"

#define WIN_SCHED_TRACE_RING_SIZE 2048
#define WIN_SCHED_MAX_CPUS 4096

typedef struct WinSchedOffsets {
    uint16_t kpcr_current_prcb;
    uint16_t prcb_current_thread;
    uint16_t kthread_apc_process;
    uint16_t kthread_client_id;
    uint16_t kthread_state;
    uint16_t ethread_thread_name;
    uint16_t eprocess_image_file_name;
} WinSchedOffsets;

typedef struct WinSchedCpuState {
    uint64_t last_thread;
    bool last_thread_valid;
} WinSchedCpuState;

typedef struct WinSchedSample {
    bool valid;
    int64_t timestamp_ns;
    int vcpu;
    uint64_t thread_ptr;
    bool have_process_ptr;
    uint64_t process_ptr;
    bool have_pid;
    uint64_t pid;
    uint64_t tid;
    bool have_state;
    uint8_t state;
    char *process_image;
    char *thread_name;
} WinSchedSample;

typedef struct WinSchedGlobalState {
    QemuMutex config_lock;
    QemuMutex ring_lock;
    int tracing_enabled;
    int offsets_ready;
    bool auto_detect;
    bool overrides_present;
    WinSchedOffsets effective;
    WindowsSchedTraceOverrides overrides;
    uint64_t kd_block;
    bool kd_attempted;
    WinSchedCpuState per_cpu[WIN_SCHED_MAX_CPUS];
    WinSchedSample ring[WIN_SCHED_TRACE_RING_SIZE];
    size_t ring_index;
    size_t ring_count;
} WinSchedGlobalState;

static WinSchedGlobalState win_sched_state;
static gsize win_sched_mutex_once;
static gsize win_sched_ring_once;

static void win_sched_init_mutexes(void)
{
    if (g_once_init_enter(&win_sched_mutex_once)) {
        qemu_mutex_init(&win_sched_state.config_lock);
        g_once_init_leave(&win_sched_mutex_once, 1);
    }
    if (g_once_init_enter(&win_sched_ring_once)) {
        qemu_mutex_init(&win_sched_state.ring_lock);
        g_once_init_leave(&win_sched_ring_once, 1);
    }
}

static inline bool win_sched_is_canonical(uint64_t value)
{
    return value == 0 || value <= 0x00007fffffffffffULL ||
           value >= 0xffff800000000000ULL;
}

static bool win_sched_read(CPUState *cpu, uint64_t addr, void *buf, size_t len)
{
    if (cpu_memory_rw_debug(cpu, addr, buf, len, false) != 0) {
        return false;
    }
    return true;
}

static bool win_sched_read_u64(CPUState *cpu, uint64_t addr, uint64_t *out)
{
    uint64_t tmp;

    if (!win_sched_read(cpu, addr, &tmp, sizeof(tmp))) {
        return false;
    }

    *out = le64_to_cpu(tmp);
    return true;
}

static char *win_sched_dup_process_image(CPUState *cpu, uint64_t process_ptr,
                                         uint16_t offset)
{
    if (!offset || !process_ptr) {
        return NULL;
    }

    char buf[16] = { 0 };

    if (!win_sched_read(cpu, process_ptr + offset, buf, sizeof(buf))) {
        return NULL;
    }

    buf[sizeof(buf) - 1] = '\0';
    if (!g_ascii_isprint(buf[0])) {
        return NULL;
    }
    return g_strdup(buf);
}

static char *win_sched_dup_thread_name(CPUState *cpu, uint64_t thread_ptr,
                                       uint16_t offset)
{
    if (!offset || !thread_ptr) {
        return NULL;
    }

    struct {
        uint16_t length;
        uint16_t maximum;
        uint32_t pad;
        uint64_t buffer;
    } __attribute__((packed)) unicode_hdr;

    if (!win_sched_read(cpu, thread_ptr + offset, &unicode_hdr,
                        sizeof(unicode_hdr))) {
        return NULL;
    }

    size_t bytes = MIN(unicode_hdr.length, unicode_hdr.maximum);
    if (bytes == 0 || bytes > 512 || !win_sched_is_canonical(unicode_hdr.buffer)) {
        return NULL;
    }

    g_autofree gunichar2 *wchars = g_new0(gunichar2, (bytes / 2) + 1);
    if (!win_sched_read(cpu, unicode_hdr.buffer, wchars, bytes)) {
        return NULL;
    }

    wchars[bytes / 2] = 0;
    g_autofree char *utf8 = g_utf16_to_utf8(wchars, -1, NULL, NULL, NULL);
    if (!utf8) {
        return NULL;
    }

    return g_steal_pointer(&utf8);
}

static void win_sched_reset_ring_locked(void)
{
    for (size_t i = 0; i < WIN_SCHED_TRACE_RING_SIZE; i++) {
        WinSchedSample *sample = &win_sched_state.ring[i];
        if (sample->valid) {
            g_free(sample->process_image);
            g_free(sample->thread_name);
        }
        memset(sample, 0, sizeof(*sample));
    }
    win_sched_state.ring_index = 0;
    win_sched_state.ring_count = 0;
}

static void win_sched_reset_cpu_state(void)
{
    for (size_t i = 0; i < WIN_SCHED_MAX_CPUS; i++) {
        win_sched_state.per_cpu[i].last_thread = 0;
        win_sched_state.per_cpu[i].last_thread_valid = false;
    }
}

static void win_sched_record_sample(const WinSchedSample *sample)
{
    if (!sample->valid) {
        return;
    }

    qemu_mutex_lock(&win_sched_state.ring_lock);

    WinSchedSample *slot = &win_sched_state.ring[win_sched_state.ring_index];
    if (slot->valid) {
        g_free(slot->process_image);
        g_free(slot->thread_name);
    }

    *slot = *sample;
    if (sample->process_image) {
        slot->process_image = g_strdup(sample->process_image);
    }
    if (sample->thread_name) {
        slot->thread_name = g_strdup(sample->thread_name);
    }

    slot->valid = true;

    win_sched_state.ring_index =
        (win_sched_state.ring_index + 1) % WIN_SCHED_TRACE_RING_SIZE;
    if (win_sched_state.ring_count < WIN_SCHED_TRACE_RING_SIZE) {
        win_sched_state.ring_count++;
    }

    qemu_mutex_unlock(&win_sched_state.ring_lock);
}

static bool win_sched_select_kdbg_pointer(CPUState *cpu, uint64_t gs_base,
                                          uint64_t *out_ptr)
{
    static const uint16_t candidates[] = { 0x120, 0x190, 0x198, 0x1a0, 0x1f8 };

    for (size_t i = 0; i < G_N_ELEMENTS(candidates); i++) {
        uint64_t ptr;

        if (!win_sched_read_u64(cpu, gs_base + candidates[i], &ptr)) {
            continue;
        }
        if (!win_sched_is_canonical(ptr) || ptr == 0) {
            continue;
        }

        KDDEBUGGER_DATA64 header;
        if (!win_sched_read(cpu, ptr, &header, sizeof(header))) {
            continue;
        }

        uint32_t tag = le32_to_cpu(header.Header.OwnerTag);
        if (tag == 0x4742444b) { /* 'KDBG' */
            *out_ptr = ptr;
            return true;
        }
    }

    return false;
}

static void win_sched_apply_overrides_locked(void)
{
    if (!win_sched_state.overrides_present) {
        return;
    }

    if (win_sched_state.overrides.has_kpcr_current_prcb) {
        win_sched_state.effective.kpcr_current_prcb =
            win_sched_state.overrides.kpcr_current_prcb;
    }
    if (win_sched_state.overrides.has_prcb_current_thread) {
        win_sched_state.effective.prcb_current_thread =
            win_sched_state.overrides.prcb_current_thread;
    }
    if (win_sched_state.overrides.has_kthread_apc_process) {
        win_sched_state.effective.kthread_apc_process =
            win_sched_state.overrides.kthread_apc_process;
    }
    if (win_sched_state.overrides.has_kthread_client_id) {
        win_sched_state.effective.kthread_client_id =
            win_sched_state.overrides.kthread_client_id;
    }
    if (win_sched_state.overrides.has_kthread_state) {
        win_sched_state.effective.kthread_state =
            win_sched_state.overrides.kthread_state;
    }
    if (win_sched_state.overrides.has_ethread_thread_name) {
        win_sched_state.effective.ethread_thread_name =
            win_sched_state.overrides.ethread_thread_name;
    }
    if (win_sched_state.overrides.has_eprocess_image_file_name) {
        win_sched_state.effective.eprocess_image_file_name =
            win_sched_state.overrides.eprocess_image_file_name;
    }
}

static void win_sched_attempt_autodetect(CPUState *cpu, X86CPU *x86_cpu)
{
    if (!win_sched_state.auto_detect || qatomic_read(&win_sched_state.offsets_ready)) {
        return;
    }

    CPUX86State *env = &x86_cpu->env;
    if ((env->hflags & HF_CPL_MASK) != 0) {
        return;
    }

    uint64_t gs_base = env->segs[R_GS].base;
    if (!gs_base) {
        return;
    }

    uint64_t kdbg_ptr = 0;
    if (!win_sched_select_kdbg_pointer(cpu, gs_base, &kdbg_ptr)) {
        return;
    }

    KDDEBUGGER_DATA64 kdbg;
    if (!win_sched_read(cpu, kdbg_ptr, &kdbg, sizeof(kdbg))) {
        return;
    }

    qemu_mutex_lock(&win_sched_state.config_lock);

    win_sched_state.kd_block = kdbg_ptr;
    win_sched_state.kd_attempted = true;

    win_sched_state.effective.kpcr_current_prcb =
        le16_to_cpu(kdbg.OffsetPcrCurrentPrcb);
    win_sched_state.effective.prcb_current_thread =
        le16_to_cpu(kdbg.OffsetPrcbCurrentThread);
    win_sched_state.effective.kthread_apc_process =
        le16_to_cpu(kdbg.OffsetKThreadApcProcess);
    win_sched_state.effective.kthread_state =
        le16_to_cpu(kdbg.OffsetKThreadState);

    win_sched_apply_overrides_locked();

    qatomic_set(&win_sched_state.offsets_ready, 1);

    qemu_mutex_unlock(&win_sched_state.config_lock);
}

static bool win_sched_build_sample(CPUState *cpu, X86CPU *x86_cpu,
                                   WinSchedSample *out)
{
    CPUX86State *env = &x86_cpu->env;
    if ((env->hflags & HF_CPL_MASK) != 0) {
        return false;
    }

    WinSchedOffsets offsets = win_sched_state.effective;

    if (cpu->cpu_index < 0 || cpu->cpu_index >= WIN_SCHED_MAX_CPUS) {
        return false;
    }

    uint64_t gs_base = env->segs[R_GS].base;
    if (!gs_base || !offsets.kpcr_current_prcb || !offsets.prcb_current_thread) {
        return false;
    }

    uint64_t prcb_ptr;
    if (!win_sched_read_u64(cpu, gs_base + offsets.kpcr_current_prcb, &prcb_ptr)) {
        return false;
    }
    if (!win_sched_is_canonical(prcb_ptr)) {
        return false;
    }

    uint64_t current_thread;
    if (!win_sched_read_u64(cpu, prcb_ptr + offsets.prcb_current_thread,
                            &current_thread)) {
        return false;
    }
    if (!win_sched_is_canonical(current_thread)) {
        return false;
    }

    if (current_thread == 0) {
        return false;
    }

    WinSchedCpuState *cpu_state = &win_sched_state.per_cpu[cpu->cpu_index];
    if (cpu_state->last_thread_valid && cpu_state->last_thread == current_thread) {
        return false;
    }

    cpu_state->last_thread = current_thread;
    cpu_state->last_thread_valid = true;

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->timestamp_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    out->vcpu = cpu->cpu_index;
    out->thread_ptr = current_thread;

    uint64_t process_ptr = 0;
    if (offsets.kthread_apc_process) {
        if (win_sched_read_u64(cpu, current_thread + offsets.kthread_apc_process,
                               &process_ptr) &&
            win_sched_is_canonical(process_ptr)) {
            out->have_process_ptr = true;
            out->process_ptr = process_ptr;
        }
    }

    if (offsets.kthread_state) {
        uint8_t state;
        if (win_sched_read(cpu, current_thread + offsets.kthread_state,
                           &state, sizeof(state))) {
            out->have_state = true;
            out->state = state;
        }
    }

    if (offsets.kthread_client_id) {
        struct {
            uint64_t unique_process;
            uint64_t unique_thread;
        } cid;

        if (win_sched_read(cpu, current_thread + offsets.kthread_client_id,
                           &cid, sizeof(cid))) {
            out->have_pid = true;
            out->pid = le64_to_cpu(cid.unique_process);
            out->tid = le64_to_cpu(cid.unique_thread);
        }
    }

    if (out->have_process_ptr) {
        out->process_image = win_sched_dup_process_image(cpu, out->process_ptr,
                                                         offsets.eprocess_image_file_name);
    }

    out->thread_name = win_sched_dup_thread_name(cpu, current_thread,
                                                 offsets.ethread_thread_name);

    return true;
}

void windows_sched_trace_post_run(CPUState *cpu)
{
    if (!cpu || !qatomic_read(&win_sched_state.tracing_enabled)) {
        return;
    }

    win_sched_init_mutexes();

    if (!object_dynamic_cast(OBJECT(cpu), TYPE_X86_CPU)) {
        return;
    }

    X86CPU *x86_cpu = X86_CPU(cpu);

    if (!qatomic_read(&win_sched_state.offsets_ready)) {
        win_sched_attempt_autodetect(cpu, x86_cpu);
    }

    if (!qatomic_read(&win_sched_state.offsets_ready)) {
        return;
    }

    WinSchedSample sample;
    if (!win_sched_build_sample(cpu, x86_cpu, &sample)) {
        return;
    }

    win_sched_record_sample(&sample);
}

WindowsSchedTraceEntryList *qmp_query_windows_sched_trace(
        bool has_max_entries, uint16_t max_entries,
        bool has_filter_vcpu, uint16_t filter_vcpu,
        bool has_filter_pid, uint64_t filter_pid,
        bool has_filter_tid, uint64_t filter_tid,
        Error **errp)
{
    if (!qatomic_read(&win_sched_state.tracing_enabled)) {
        return NULL;
    }

    win_sched_init_mutexes();

    WindowsSchedTraceEntryList *head = NULL;
    size_t produced = 0;

    qemu_mutex_lock(&win_sched_state.ring_lock);

    size_t remaining = win_sched_state.ring_count;
    size_t idx = (win_sched_state.ring_index + WIN_SCHED_TRACE_RING_SIZE - 1) %
                 WIN_SCHED_TRACE_RING_SIZE;

    while (remaining > 0) {
        WinSchedSample *sample = &win_sched_state.ring[idx];

        if (sample->valid) {
            bool match = true;
            if (has_filter_vcpu && sample->vcpu != filter_vcpu) {
                match = false;
            }
            if (match && has_filter_pid && (!sample->have_pid || sample->pid != filter_pid)) {
                match = false;
            }
            if (match && has_filter_tid && (!sample->have_pid || sample->tid != filter_tid)) {
                match = false;
            }

            if (match) {
                if (!has_max_entries || produced < max_entries) {
                    WindowsSchedTraceEntry *entry = g_new0(WindowsSchedTraceEntry, 1);
                    entry->timestamp_ns = sample->timestamp_ns;
                    entry->vcpu = sample->vcpu;
                    entry->thread_pointer = sample->thread_ptr;

                    if (sample->have_process_ptr) {
                        entry->has_process_pointer = true;
                        entry->process_pointer = sample->process_ptr;
                    }
                    if (sample->have_pid) {
                        entry->has_unique_process_id = true;
                        entry->unique_process_id = sample->pid;
                        entry->has_unique_thread_id = true;
                        entry->unique_thread_id = sample->tid;
                    }
                    if (sample->have_state) {
                        entry->has_kthread_state = true;
                        entry->kthread_state = sample->state;
                    }
                    if (sample->process_image) {
                        entry->process_image = g_strdup(sample->process_image);
                    }
                    if (sample->thread_name) {
                        entry->thread_name = g_strdup(sample->thread_name);
                    }

                    QAPI_LIST_PREPEND(head, entry);
                    produced++;
                } else {
                    break;
                }
            }
        }

        if (idx == 0) {
            idx = WIN_SCHED_TRACE_RING_SIZE - 1;
        } else {
            idx--;
        }
        remaining--;
    }

    qemu_mutex_unlock(&win_sched_state.ring_lock);

    return head;
}

static void win_sched_disable_locked(void)
{
    qatomic_set(&win_sched_state.tracing_enabled, 0);
    qatomic_set(&win_sched_state.offsets_ready, 0);
    win_sched_state.kd_block = 0;
    win_sched_state.kd_attempted = false;
    win_sched_state.auto_detect = true;
    memset(&win_sched_state.effective, 0, sizeof(win_sched_state.effective));

    qemu_mutex_lock(&win_sched_state.ring_lock);
    win_sched_reset_ring_locked();
    qemu_mutex_unlock(&win_sched_state.ring_lock);

    win_sched_reset_cpu_state();
}

void qmp_windows_sched_trace_set(bool enable,
                                 bool has_auto_detect, bool auto_detect,
                                 WindowsSchedTraceOverrides *overrides,
                                 Error **errp)
{
    win_sched_init_mutexes();

    if (enable) {
        if (!first_cpu || !object_dynamic_cast(OBJECT(first_cpu), TYPE_X86_CPU)) {
            error_setg(errp, "Windows scheduler tracing requires an x86 guest");
            goto out;
        }
    }

    qemu_mutex_lock(&win_sched_state.config_lock);

    if (!enable) {
        win_sched_disable_locked();
        goto unlock;
    }

    win_sched_disable_locked();

    win_sched_state.auto_detect = has_auto_detect ? auto_detect : true;

    memset(&win_sched_state.overrides, 0, sizeof(win_sched_state.overrides));
    win_sched_state.overrides_present = overrides != NULL;
    if (overrides) {
        win_sched_state.overrides = *overrides;
    }

    win_sched_apply_overrides_locked();

    if (!win_sched_state.auto_detect &&
        win_sched_state.effective.kpcr_current_prcb &&
        win_sched_state.effective.prcb_current_thread) {
        qatomic_set(&win_sched_state.offsets_ready, 1);
    }

    qatomic_set(&win_sched_state.tracing_enabled, 1);

unlock:
    qemu_mutex_unlock(&win_sched_state.config_lock);

out:
    if (overrides) {
        qapi_free_WindowsSchedTraceOverrides(overrides);
    }
}
