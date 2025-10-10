// time.h  — structured timing/logging API (drop-in)
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- Log levels ----
typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

// ---- Timing context ----
typedef struct TimingContext {
    char     name[128];
    uint64_t start_ticks;  // ns (CLOCK_MONOTONIC[_RAW])
    uint64_t end_ticks;    // ns
} TimingContext;

// ---- Init / cleanup ----
int  timing_init(const char *log_file_path);  // returns 0 on success
void timing_cleanup(void);

// ---- Ticks & conversions ----
uint64_t timing_get_ticks(void);      // ns
double   timing_ticks_to_us(uint64_t ticks);
double   timing_ticks_to_ms(uint64_t ticks);
double   timing_ticks_to_sec(uint64_t ticks);

// ---- Timestamp helpers ----
void timing_get_timestamp_str(char *buffer, size_t size); // "YYYY-MM-DD HH:MM:SS.mmm"

// ---- Logging (thread-safe) ----
void timing_log(LogLevel level, const char *fmt, ...);

// ---- High-level measures ----
TimingContext* timing_start(const char *label);  // malloc
double         timing_end(TimingContext *ctx);   // logs & free, returns ms
double         timing_get_elapsed_ms(const TimingContext *ctx);

// ---- Benchmark helper ----
void timing_benchmark(const char *name, void (*func)(void), int iterations);

// ---- Convenience macros ----
#define TIMING_START(name) timing_start(name)
#define TIMING_END(ctx)    (void)timing_end(ctx)
#define TIMING_LOG_INFO(fmt, ...)  timing_log(LOG_INFO,  fmt, ##__VA_ARGS__)
#define TIMING_LOG_ERROR(fmt, ...) timing_log(LOG_ERROR, fmt, ##__VA_ARGS__)

// ---- Scoped timing ----
#define TIMING_MEASURE(name, code_block) do {           \
    TimingContext *_tctx = timing_start(name);          \
    code_block                                          \
    (void)timing_end(_tctx);                            \
} while (0)
