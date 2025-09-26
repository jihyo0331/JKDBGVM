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

void qemu_set_irq(qemu_irq irq, int level)
{
    if (!irq)
        return;

    if (qatomic_read(&irq_log_enabled)) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        char *path = object_get_canonical_path(OBJECT(irq));

        int thread_id = qemu_get_thread_id();
        void *caller = __builtin_return_address(0);

        error_printf("irq-log: time=%" PRId64 "ns level=%d n=%d kind=%s\n"
                     "         path=%s\n"
                     "         irq=%p handler=%p opaque=%p\n"
                     "         host-tid=%d caller=%p\n",
                     now, level, irq->n, irq_classification(irq),
                     path ? path : "(anonymous)", irq, irq->handler,
                     irq->opaque, thread_id, caller);
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
