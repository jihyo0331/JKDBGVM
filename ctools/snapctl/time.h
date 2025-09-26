#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>
#include <stdio.h>

// Log levels
typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

// Timing context for measuring operations
typedef struct {
    char name[128];
    uint64_t start_ticks;
    uint64_t end_ticks;
} TimingContext;

// Initialization and cleanup
int timing_init(const char *log_file_path);
void timing_cleanup(void);

// Low-level timing functions
uint64_t timing_get_ticks(void);
double timing_ticks_to_us(uint64_t ticks);
double timing_ticks_to_ms(uint64_t ticks);
double timing_ticks_to_sec(uint64_t ticks);

// Timestamp utilities
void timing_get_timestamp_str(char *buffer, size_t size);

// Logging with timing
void timing_log(LogLevel level, const char *format, ...);

// High-level timing measurement
TimingContext* timing_start(const char *operation_name);
double timing_end(TimingContext *ctx);
double timing_get_elapsed_ms(const TimingContext *ctx);

// Benchmarking
void timing_benchmark(const char *name, void (*func)(void), int iterations);

// Convenience macros
#define TIMING_START(name) timing_start(name)
#define TIMING_END(ctx) (void)timing_end(ctx)
#define TIMING_LOG_INFO(fmt, ...) timing_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define TIMING_LOG_ERROR(fmt, ...) timing_log(LOG_ERROR, fmt, ##__VA_ARGS__)

// Automatic timing for code blocks
#define TIMING_MEASURE(name, code_block) do { \
    TimingContext *_timing_ctx = timing_start(name); \
    code_block \
    (void)timing_end(_timing_ctx); \
} while(0)

#endif // TIMING_H
