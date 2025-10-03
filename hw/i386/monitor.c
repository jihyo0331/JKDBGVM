/*
 * QEMU monitor
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include "monitor/monitor.h"
#include "qobject/qdict.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"
#include "hw/i386/x86.h"
#include "hw/rtc/mc146818rtc.h"
#include "exec/icount.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "qemu/atomic.h"
#include "qemu/seqlock.h"
#include "qemu/timer.h"
#include "system/cpu-timers-internal.h"

#include CONFIG_DEVICES

void qmp_rtc_reset_reinjection(Error **errp)
{
    X86MachineState *x86ms = X86_MACHINE(qdev_get_machine());

#ifdef CONFIG_MC146818RTC
    if (x86ms->rtc) {
        rtc_reset_reinjection(MC146818_RTC(x86ms->rtc));
    }
#else
    assert(!x86ms->rtc);
#endif
}

void qmp_x_tcg_set_icount_shift(int64_t value, Error **errp)
{
#ifndef CONFIG_TCG
    error_setg(errp, "TCG accelerator is not available in this build");
    return;
#else
    int16_t old_shift;
    int16_t new_shift;
    bool was_running;
    int64_t current_ns;
    int64_t raw_icount;

    if (!tcg_enabled()) {
        error_setg(errp, "TCG accelerator must be active to change icount shift");
        return;
    }

    if (icount_enabled() != ICOUNT_PRECISE) {
        error_setg(errp, "icount shift can only be adjusted in precise mode");
        return;
    }

    if (value < 0 || value > ICOUNT_SHIFT_MAX) {
        error_setg(errp, "value must be in the range [0, %d]", ICOUNT_SHIFT_MAX);
        return;
    }

    new_shift = value;
    old_shift = qatomic_read(&timers_state.icount_time_shift);
    if (new_shift == old_shift) {
        return;
    }

    was_running = runstate_is_running();
    if (was_running) {
        pause_all_vcpus();
    }

    current_ns = icount_get();

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    qatomic_set(&timers_state.icount_time_shift, new_shift);
    raw_icount = qatomic_read_i64(&timers_state.qemu_icount);
    qatomic_set_i64(&timers_state.qemu_icount_bias,
                    current_ns - (raw_icount << new_shift));
    timers_state.last_delta = 0;
    timers_state.vm_clock_warp_start = -1;
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);

    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);

    if (was_running) {
        resume_all_vcpus();
    }
#endif
}
