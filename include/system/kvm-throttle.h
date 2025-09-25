#pragma once
#include "qemu/osdep.h"
#include "qemu/typedefs.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"

typedef struct ThrottleCfg {
    bool     enabled;
    uint32_t percent;        // 0..100
    int64_t  period_ns;      // e.g. 20ms
    // 주기 창
    int64_t  window_start_ns;
    int64_t  window_end_ns;
    int64_t  on_ns;          // period_ns * percent / 100
    int64_t  budget_ns;      // 남은 실행 가능 시간(ns)
    int64_t  last_check_ns;  // 모노토닉 기준 마지막 점검 시각
    int64_t  thread_last_ns; // CLOCK_THREAD_CPUTIME_ID 기준 마지막 측정치
    bool     thread_time_valid;
    QEMUTimer *on_timer;
} ThrottleCfg;

static inline int64_t mono_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline void ns_to_ts(int64_t ns, struct timespec *ts) {
    ts->tv_sec  = ns / 1000000000LL;
    ts->tv_nsec = ns % 1000000000LL;
}

/* per-CPU 스로틀 상태 접근자: CPUState 안에 여분 필드가 없으므로
 * 간단히 cpu->opaque2 같은 곳을 쓸 수 없다면, 별도 배열로 두거나
 * 여기서는 일단 CPUState에 sidecar 포인터 하나를 캐싱하는 전역 해시로 구현해도 됨.
 * 최소 패치 버전에선 static GArray/동적 배열을 인덱스로 씀.
 */
ThrottleCfg *kvm_thr_get(CPUState *cpu);
void kvm_thr_set_all(int cpu_index /* -1=all */, int percent, int period_ms);
void kvm_thr_tick_before_exec(CPUState *cpu);
