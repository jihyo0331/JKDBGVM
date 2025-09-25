#include "system/kvm-throttle.h"
#include "qemu/rcu.h"
#include "qemu/typedefs.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "system/cpus.h"
#include "qemu/timer.h"          /* QEMUTimer, timer_new_ns, timer_mod_ns */
#include <linux/kvm.h>
#include <errno.h>

static inline int64_t thread_time_now_ns(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec ts;

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
#endif
    return -1;
}

static void sleep_until_ns(int64_t deadline_ns)
{
    struct timespec ts;
    int ret;

    do {
        ns_to_ts(deadline_ns, &ts);
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (ret == EINTR);
}

static ThrottleCfg *g_thr;       // cpu_index 기반 동적 배열
static int g_thr_cap = 0;

/* cpu_index가 들어오면, 해당 인덱스까지 배열을 확장 */
static inline void ensure_index(int need_idx) {
    if (need_idx < g_thr_cap) return;
    int new_cap = need_idx + 1;
    g_thr = g_realloc_n(g_thr, new_cap, sizeof(*g_thr));
    for (int i = g_thr_cap; i < new_cap; i++) {
        g_thr[i] = (ThrottleCfg){
            .enabled = false,
            .percent = 100,
            .period_ns = 20000000, /* 20ms */
            .window_start_ns = 0,
            .window_end_ns = 0,
            .on_ns = 0,
            .budget_ns = 0,
            .last_check_ns = 0,
            .thread_last_ns = 0,
            .thread_time_valid = false,
        };
    }
    g_thr_cap = new_cap;
}

static void kvm_thr_on_expire(void *opaque)
{
    CPUState *cpu = opaque;
    if (cpu->kvm_run) {
        cpu->kvm_run->immediate_exit = 1;  // 현재 KVM_RUN을 즉시 탈출
        smp_wmb();
        qemu_cpu_kick(cpu);
    }
}

ThrottleCfg *kvm_thr_get(CPUState *cpu) {
    ensure_index(cpu->cpu_index);
    ThrottleCfg *t = &g_thr[cpu->cpu_index];
   if (unlikely(!t->on_timer)) {
       t->on_timer = timer_new_ns(QEMU_CLOCK_HOST, kvm_thr_on_expire, cpu);
   }
   return t;
}

void kvm_thr_set_all(int cpu_index, int percent, int period_ms) {
    int64_t now = mono_now_ns();
    int64_t per = (int64_t)period_ms * 1000000LL;

    CPUState *cs;
    CPU_FOREACH(cs) {
        if (cpu_index >= 0 && cs->cpu_index != cpu_index) {
            continue;
        }
        ThrottleCfg *t = kvm_thr_get(cs); /* ensure_index 내부에서 확장 */
        t->percent    = percent;
        t->period_ns  = per;
        t->enabled    = (percent < 100);
        t->window_start_ns = now;
        t->window_end_ns   = now + per;
        t->on_ns      = (per * percent) / 100;
        t->budget_ns  = t->on_ns;
        t->last_check_ns = now;
        t->thread_last_ns = 0;
        t->thread_time_valid = false;
        if (t->on_timer) {
            timer_del(t->on_timer);
        }
        // 메모리 장벽(다른 스레드가 곧바로 새 값을 보게)
        smp_wmb();
    }
}

/* 토큰 버킷 기반: 주기마다 on_ns 만큼 실행하고, 초과분은 window_end까지 쉼 */
void kvm_thr_tick_before_exec(CPUState *cpu)
{
    ThrottleCfg *t = kvm_thr_get(cpu);

    if (unlikely(!t->enabled || t->percent >= 100)) {
        return;
    }

    for (;;) {
        int64_t now = mono_now_ns();
        int64_t cpu_now = thread_time_now_ns();

        if (now >= t->window_end_ns) {
            /* 누락된 기간만큼 주기 경계 보정 (드리프트 억제) */
            int64_t span = now - t->window_start_ns;
            int64_t step = (span / t->period_ns) + 1;

            t->window_start_ns += step * t->period_ns;
            t->window_end_ns    = t->window_start_ns + t->period_ns;
            t->budget_ns        = t->on_ns;
            t->last_check_ns    = now;
            t->thread_time_valid = false;
            continue;
        }

        if (cpu_now >= 0) {
            if (t->thread_time_valid) {
                int64_t delta = cpu_now - t->thread_last_ns;

                if (delta > 0) {
                    t->budget_ns -= delta;
                }
            }
            t->thread_last_ns = cpu_now;
            t->thread_time_valid = true;
        } else if (t->last_check_ns) {
            int64_t delta = now - t->last_check_ns;

            if (delta > 0) {
                t->budget_ns -= delta;
            }
        }
        t->last_check_ns = now;

        if (t->budget_ns <= 0) {
            int64_t sleep_until = t->window_end_ns;

            if (sleep_until > now) {
                sleep_until_ns(sleep_until);
            }

            t->window_start_ns = t->window_end_ns;
            t->window_end_ns   = t->window_start_ns + t->period_ns;
            t->budget_ns       = t->on_ns;
            if (cpu_now >= 0) {
                int64_t refresh = thread_time_now_ns();

                if (refresh >= 0) {
                    t->thread_last_ns = refresh;
                    t->thread_time_valid = true;
                } else {
                    t->thread_time_valid = false;
                    t->last_check_ns = t->window_start_ns;
                }
            } else {
                t->thread_time_valid = false;
                t->last_check_ns = t->window_start_ns;
            }
            continue;
        }

        if (t->on_timer) {
            timer_mod_ns(t->on_timer, now + t->budget_ns);
        }
        return;
    }
}
