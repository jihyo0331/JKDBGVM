/*
 * QEMU IRQ/GPIO common code.
 *
 * Copyright (c) 2007 CodeSourcery.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "disas/disas.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/type-helpers.h"
#include "hw/irq.h"
#include "qom/object.h"
#ifdef CONFIG_ARM_GIC
#include "hw/intc/arm_gic_common.h"
#endif
#include "qemu/timer.h"

static const char *irq_classification(IRQState *irq)
{
#ifdef CONFIG_ARM_GIC
    for (Object *obj = OBJECT(irq); obj; obj = obj->parent) {
        if (object_dynamic_cast(obj, TYPE_ARM_GIC_COMMON)) {
            if (irq->n < 16) {
                return "software (SGI)";
            }
            if (irq->n < 32) {
                return "percpu (PPI)";
            }

            return "hardware (SPI)";
        }
    }
#endif

    return "hardware";
}

static int irq_log_enabled;

#if defined(__GNUC__)
#pragma weak lookup_symbol
#endif

#define IRQ_LOG_RING_SIZE 1024

typedef struct IRQTraceSample {
    bool valid;
    int64_t timestamp_ns;
    int level;
    int irq_line;
    char *kind;
    char *path;
    int host_tid;
    char *thread_name;
    uint64_t caller_addr;
    char *caller_symbol;
} IRQTraceSample;

static IRQTraceSample irq_trace_ring[IRQ_LOG_RING_SIZE];
static size_t irq_trace_index;
static size_t irq_trace_count;
static QemuMutex irq_trace_mutex;
static gsize irq_trace_mutex_once;

static char *irq_trace_lookup_thread_name(int tid)
{
#ifdef __linux__
    char *filename = g_strdup_printf("/proc/self/task/%d/comm", tid);
    char *contents = NULL;
    gsize len;

    if (g_file_get_contents(filename, &contents, &len, NULL)) {
        g_strchomp(contents);
    }
    g_free(filename);
    return contents;
#else
    (void)tid;
    return NULL;
#endif
}

static void irq_trace_entry_reset(IRQTraceSample *entry)
{
    if (!entry->valid) {
        return;
    }

    g_free(entry->kind);
    g_free(entry->path);
    g_free(entry->thread_name);
    g_free(entry->caller_symbol);
    memset(entry, 0, sizeof(*entry));
}

static void irq_trace_ensure_init(void)
{
    if (g_once_init_enter(&irq_trace_mutex_once)) {
        qemu_mutex_init(&irq_trace_mutex);
        g_once_init_leave(&irq_trace_mutex_once, 1);
    }
}

static void irq_trace_record(int64_t timestamp_ns, int level, int irq_line,
                             const char *kind, const char *path,
                             int host_tid, uint64_t caller_addr,
                             const char *caller_symbol)
{
    irq_trace_ensure_init();
    qemu_mutex_lock(&irq_trace_mutex);

    IRQTraceSample *slot = &irq_trace_ring[irq_trace_index];
    irq_trace_entry_reset(slot);

    slot->timestamp_ns = timestamp_ns;
    slot->level = level;
    slot->irq_line = irq_line;
    slot->kind = g_strdup(kind);
    slot->path = g_strdup(path);
    slot->host_tid = host_tid;
    slot->thread_name = irq_trace_lookup_thread_name(host_tid);
    slot->caller_addr = caller_addr;
    if (caller_symbol && *caller_symbol) {
        slot->caller_symbol = g_strdup(caller_symbol);
    }
    slot->valid = true;

    irq_trace_index = (irq_trace_index + 1) % IRQ_LOG_RING_SIZE;
    if (irq_trace_count < IRQ_LOG_RING_SIZE) {
        irq_trace_count++;
    }

    qemu_mutex_unlock(&irq_trace_mutex);
}

void qemu_set_irq(qemu_irq irq, int level)
{
    if (!irq)
        return;

    if (qatomic_read(&irq_log_enabled)) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        char *path = object_get_canonical_path(OBJECT(irq));
        const char *classification = irq_classification(irq);
        int thread_id = qemu_get_thread_id();
        void *caller = __builtin_return_address(0);
        const char *symbol = NULL;
        if (lookup_symbol) {
            symbol = lookup_symbol((uint64_t)caller);
        }
        const char *path_or_default = path ? path : "(anonymous)";

        error_printf("irq-log: time=%" PRId64 "ns level=%d n=%d kind=%s\n"
                     "         path=%s\n"
                     "         irq=%p handler=%p opaque=%p\n"
                     "         host-tid=%d caller=%p\n",
                     now, level, irq->n, classification,
                     path_or_default, irq, irq->handler,
                     irq->opaque, thread_id, caller);

        irq_trace_record(now, level, irq->n, classification,
                         path_or_default, thread_id,
                         (uint64_t)caller, symbol && symbol[0] ? symbol : NULL);
        g_free(path);
    }

    irq->handler(irq->opaque, irq->n, level);
}

void qemu_irq_log_set_enabled(bool enable)
{
    qatomic_set(&irq_log_enabled, enable);
    error_printf("irq-log: %s\n", enable ? "enabled" : "disabled");
}

bool qemu_irq_log_enabled(void)
{
    return qatomic_read(&irq_log_enabled);
}

IRQTraceEntryList *qmp_query_irq_log(bool has_max_entries, uint16_t max_entries,
                                     bool has_filter_tid, int64_t filter_tid,
                                     bool has_filter_line, int64_t filter_line,
                                     Error **errp)
{
    IRQTraceEntryList *head = NULL;
    size_t produced = 0;

    irq_trace_ensure_init();
    qemu_mutex_lock(&irq_trace_mutex);

    size_t remaining = irq_trace_count;
    size_t idx = (irq_trace_index + IRQ_LOG_RING_SIZE - 1) % IRQ_LOG_RING_SIZE;

    while (remaining > 0) {
        IRQTraceSample *sample = &irq_trace_ring[idx];

        if (sample->valid) {
            bool match = true;

            if (has_filter_tid && sample->host_tid != filter_tid) {
                match = false;
            }
            if (match && has_filter_line && sample->irq_line != filter_line) {
                match = false;
            }

            if (match) {
                if (!has_max_entries || produced < max_entries) {
                    IRQTraceEntry *entry = g_new0(IRQTraceEntry, 1);

                    entry->timestamp_ns = sample->timestamp_ns;
                    entry->level = sample->level;
                    entry->irq_line = sample->irq_line;
                    entry->kind = g_strdup(sample->kind ? sample->kind : "");
                    entry->path = g_strdup(sample->path ? sample->path : "");
                    entry->host_tid = sample->host_tid;
                    entry->caller_addr = sample->caller_addr;

                    if (sample->thread_name) {
                        entry->thread_name = g_strdup(sample->thread_name);
                    }
                    if (sample->caller_symbol) {
                        entry->caller_symbol = g_strdup(sample->caller_symbol);
                    }

                    QAPI_LIST_PREPEND(head, entry);
                    produced++;
                } else {
                    break;
                }
            }
        }

        if (idx == 0) {
            idx = IRQ_LOG_RING_SIZE - 1;
        } else {
            idx--;
        }
        remaining--;
    }

    qemu_mutex_unlock(&irq_trace_mutex);

    return head;
}

static void init_irq_fields(IRQState *irq, qemu_irq_handler handler,
                            void *opaque, int n)
{
    irq->handler = handler;
    irq->opaque = opaque;
    irq->n = n;
}

void qemu_init_irq(IRQState *irq, qemu_irq_handler handler, void *opaque,
                   int n)
{
    object_initialize(irq, sizeof(*irq), TYPE_IRQ);
    init_irq_fields(irq, handler, opaque, n);
}

void qemu_init_irq_child(Object *parent, const char *propname,
                         IRQState *irq, qemu_irq_handler handler,
                         void *opaque, int n)
{
    object_initialize_child(parent, propname, irq, TYPE_IRQ);
    init_irq_fields(irq, handler, opaque, n);
}

void qemu_init_irqs(IRQState irq[], size_t count,
                    qemu_irq_handler handler, void *opaque)
{
    for (size_t i = 0; i < count; i++) {
        qemu_init_irq(&irq[i], handler, opaque, i);
    }
}

qemu_irq *qemu_extend_irqs(qemu_irq *old, int n_old, qemu_irq_handler handler,
                           void *opaque, int n)
{
    qemu_irq *s;
    int i;

    if (!old) {
        n_old = 0;
    }
    s = old ? g_renew(qemu_irq, old, n + n_old) : g_new(qemu_irq, n);
    for (i = n_old; i < n + n_old; i++) {
        s[i] = qemu_allocate_irq(handler, opaque, i);
    }
    return s;
}

qemu_irq *qemu_allocate_irqs(qemu_irq_handler handler, void *opaque, int n)
{
    return qemu_extend_irqs(NULL, 0, handler, opaque, n);
}

qemu_irq qemu_allocate_irq(qemu_irq_handler handler, void *opaque, int n)
{
    IRQState *irq = IRQ(object_new(TYPE_IRQ));
    init_irq_fields(irq, handler, opaque, n);
    return irq;
}

void qemu_free_irqs(qemu_irq *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        qemu_free_irq(s[i]);
    }
    g_free(s);
}

void qemu_free_irq(qemu_irq irq)
{
    object_unref(OBJECT(irq));
}

static void qemu_notirq(void *opaque, int line, int level)
{
    IRQState *irq = opaque;

    irq->handler(irq->opaque, irq->n, !level);
}

qemu_irq qemu_irq_invert(qemu_irq irq)
{
    /* The default state for IRQs is low, so raise the output now.  */
    qemu_irq_raise(irq);
    return qemu_allocate_irq(qemu_notirq, irq, 0);
}

void qemu_irq_intercept_in(qemu_irq *gpio_in, qemu_irq_handler handler, int n)
{
    int i;
    qemu_irq *old_irqs = qemu_allocate_irqs(NULL, NULL, n);
    for (i = 0; i < n; i++) {
        *old_irqs[i] = *gpio_in[i];
        gpio_in[i]->handler = handler;
        gpio_in[i]->opaque = &old_irqs[i];
    }
}

static const TypeInfo irq_type_info = {
   .name = TYPE_IRQ,
   .parent = TYPE_OBJECT,
   .instance_size = sizeof(IRQState),
};

static void irq_register_types(void)
{
    type_register_static(&irq_type_info);
}

type_init(irq_register_types)
