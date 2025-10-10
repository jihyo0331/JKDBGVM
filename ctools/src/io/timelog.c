#define _POSIX_C_SOURCE 200809L
#include "timelog.h" 

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <errno.h>

static FILE *timing_log_fp = NULL;

static const char *log_level_to_string(LogLevel level) {
    switch (level) {
    case LOG_TRACE: return "TRACE";
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "INFO";
    }
}

void timing_log(LogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char timestamp[32];
    timing_get_timestamp_str(timestamp, sizeof(timestamp));
    const char *level_str = log_level_to_string(level);

    if (timing_log_fp) {
        va_list ap_file;
        va_copy(ap_file, ap);
        fprintf(timing_log_fp, "[%s] [%s] ", timestamp, level_str);
        vfprintf(timing_log_fp, fmt, ap_file);
        fprintf(timing_log_fp, "\n");
        fflush(timing_log_fp);
        va_end(ap_file);
    }

    FILE *console = timing_log_fp ? stdout : stderr;
    va_list ap_console;
    va_copy(ap_console, ap);
    fprintf(console, "[%s] [%s] ", timestamp, level_str);
    vfprintf(console, fmt, ap_console);
    fprintf(console, "\n");
    fflush(console);
    va_end(ap_console);

    va_end(ap);
}

void timing_get_timestamp_str(char *buffer, size_t size) {
    if (!buffer || size == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_now = localtime_r(&now, &tm_buf);
    if (!tm_now) {
        buffer[0] = '\0';
        return;
    }

    if (strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_now) == 0) {
        buffer[0] = '\0';
    }
}

int timing_init(const char *log_path) {
    if (timing_log_fp) {
        fclose(timing_log_fp);
        timing_log_fp = NULL;
    }

    if (!log_path || !*log_path) {
        return 0;
    }

    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        fprintf(stderr, "timing_init: failed to open %s: %s\n",
                log_path, strerror(errno));
        return -1;
    }

    setvbuf(fp, NULL, _IOLBF, 0);
    timing_log_fp = fp;
    return 0;
}

void timing_cleanup(void) {
    if (timing_log_fp) {
        fflush(timing_log_fp);
        fclose(timing_log_fp);
        timing_log_fp = NULL;
    }
}

uint64_t timing_get_ticks(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        timing_log(LOG_ERROR, "clock_gettime failed: %s", strerror(errno));
        return 0;
    }

    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

double timing_ticks_to_us(uint64_t ticks) {
    return ticks / 1000.0;
}

double timing_ticks_to_ms(uint64_t ticks) {
    return ticks / 1000000.0;
}

double timing_ticks_to_sec(uint64_t ticks) {
    return ticks / 1000000000.0;
}

TimingContext* timing_start(const char *operation_name) {
    TimingContext *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        timing_log(LOG_ERROR, "Failed to allocate TimingContext");
        return NULL;
    }

    const char *name = operation_name ? operation_name : "(unnamed)";
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);
    ctx->start_ticks = timing_get_ticks();
    ctx->end_ticks = 0;

    timing_log(LOG_DEBUG, "Timing start: %s", ctx->name);
    return ctx;
}

double timing_end(TimingContext *ctx) {
    if (!ctx) {
        return 0.0;
    }

    ctx->end_ticks = timing_get_ticks();
    uint64_t delta = 0;
    if (ctx->end_ticks >= ctx->start_ticks) {
        delta = ctx->end_ticks - ctx->start_ticks;
    }

    double elapsed_ms = timing_ticks_to_ms(delta);
    double elapsed_s = elapsed_ms / 1000.0;
    timing_log(LOG_DEBUG, "%s: %.3f s (%.0f ms)", ctx->name, elapsed_s, elapsed_ms);
    free(ctx);
    return elapsed_s;
}

double timing_get_elapsed_ms(const TimingContext *ctx) {
    if (!ctx) {
        return 0.0;
    }

    uint64_t end = ctx->end_ticks ? ctx->end_ticks : timing_get_ticks();
    if (end < ctx->start_ticks) {
        return 0.0;
    }

    return timing_ticks_to_ms(end - ctx->start_ticks);
}

void timing_benchmark(const char *name, void (*func)(void), int iterations) {
    if (!func || iterations <= 0) {
        timing_log(LOG_WARN, "Benchmark '%s' skipped (invalid parameters)",
                   name ? name : "(unnamed)");
        return;
    }

    double total_ms = 0.0;
    double best_ms = DBL_MAX;
    double worst_ms = 0.0;
    int recorded = 0;

    for (int i = 0; i < iterations; ++i) {
        uint64_t start = timing_get_ticks();
        func();
        uint64_t end = timing_get_ticks();

        if (end < start) {
            timing_log(LOG_WARN, "tick counter wrapped during benchmark");
            continue;
        }

        double elapsed_ms = timing_ticks_to_ms(end - start);
        total_ms += elapsed_ms;
        recorded++;
        if (elapsed_ms < best_ms) {
            best_ms = elapsed_ms;
        }
        if (elapsed_ms > worst_ms) {
            worst_ms = elapsed_ms;
        }
    }

    if (recorded == 0) {
        timing_log(LOG_WARN, "Benchmark '%s' produced no samples",
                   name ? name : "(unnamed)");
        return;
    }

    double avg_ms = total_ms / recorded;
    timing_log(LOG_INFO,
               "Benchmark '%s': avg %.3f ms (best %.3f ms, worst %.3f ms) over %d iterations",
               name ? name : "(unnamed)", avg_ms, best_ms, worst_ms, recorded);
}

#ifdef TIMING_STANDALONE
static void sleep_us(unsigned int usec) {
    struct timespec req;
    req.tv_sec = usec / 1000000U;
    req.tv_nsec = (long)(usec % 1000000U) * 1000L;

    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        continue;
    }
}

// Test functions for benchmarking
static void test_cpu_intensive(void) {
    volatile int sum = 0;
    for (int i = 0; i < 1000000; i++) {
        sum += i * i;
    }
}

static void test_memory_allocation(void) {
    char *ptrs[1000];
    for (int i = 0; i < 1000; i++) {
        ptrs[i] = malloc(1024);
        memset(ptrs[i], i & 0xFF, 1024);
    }
    for (int i = 0; i < 1000; i++) {
        free(ptrs[i]);
    }
}

static void test_file_operations(void) {
    FILE *fp = fopen("/tmp/timing_test.tmp", "w");
    if (fp) {
        for (int i = 0; i < 1000; i++) {
            fprintf(fp, "Line %d: This is a test line for timing\n", i);
        }
        fclose(fp);
    }
    
    fp = fopen("/tmp/timing_test.tmp", "r");
    if (fp) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp)) {
            // Just read the file
        }
        fclose(fp);
    }
    
    unlink("/tmp/timing_test.tmp");
}

static void test_sleep(void) {
    sleep_us(10000); // 10ms
}

static void demonstrate_timing_api(void) {
    printf("=== Timing API Demonstration ===\n");
    
    // Basic timing measurement
    TimingContext *ctx = timing_start("Basic operation demo");
    sleep_us(50000); // 50ms
    timing_end(ctx);
    
    // Nested timing
    TimingContext *outer = timing_start("Outer operation");
    {
        TimingContext *inner1 = timing_start("Inner operation 1");
        test_cpu_intensive();
        timing_end(inner1);
        
        TimingContext *inner2 = timing_start("Inner operation 2");
        sleep_us(20000); // 20ms
        timing_end(inner2);
    }
    timing_end(outer);
    
    // Using the macro
    TIMING_MEASURE("Macro test", {
        test_memory_allocation();
    });
    
    // Progress monitoring
    TimingContext *progress_ctx = timing_start("Progress monitoring demo");
    for (int i = 0; i < 5; i++) {
        sleep_us(100000); // 100ms
        double elapsed = timing_get_elapsed_ms(progress_ctx);
        timing_log(LOG_INFO, "Progress: step %d/5, elapsed: %.1f ms", i+1, elapsed);
    }
    timing_end(progress_ctx);
    
    printf("=== API Demonstration Complete ===\n\n");
}

static void run_comprehensive_benchmarks(void) {
    printf("=== Comprehensive Timing Benchmarks ===\n");
    
    timing_benchmark("CPU intensive task", test_cpu_intensive, 10);
    timing_benchmark("Memory allocation", test_memory_allocation, 20);
    timing_benchmark("File I/O operations", test_file_operations, 5);
    timing_benchmark("Sleep/scheduling", test_sleep, 10);
    
    printf("=== Benchmarks Complete ===\n\n");
}

static void test_precision_comparison(void) {
    printf("=== Timing Precision Comparison ===\n");
    
    // Test RDTSC vs clock_gettime precision
    uint64_t start_tsc = timing_get_ticks();
    struct timespec start_clock;
    clock_gettime(CLOCK_MONOTONIC, &start_clock);
    
    // Short operation
    volatile int dummy = 0;
    for (int i = 0; i < 1000; i++) {
        dummy += i;
    }
    
    uint64_t end_tsc = timing_get_ticks();
    struct timespec end_clock;
    clock_gettime(CLOCK_MONOTONIC, &end_clock);
    
    uint64_t tsc_diff = end_tsc - start_tsc;
    double tsc_us = timing_ticks_to_us(tsc_diff);
    
    double clock_us = (end_clock.tv_sec - start_clock.tv_sec) * 1e6 + 
                     (end_clock.tv_nsec - start_clock.tv_nsec) / 1000.0;
    
    timing_log(LOG_INFO, "Short operation timing:");
    timing_log(LOG_INFO, "  RDTSC: %.3f μs", tsc_us);
    timing_log(LOG_INFO, "  clock_gettime: %.3f μs", clock_us);
    timing_log(LOG_INFO, "  Difference: %.3f μs", fabs(tsc_us - clock_us));
    
    printf("=== Precision Comparison Complete ===\n\n");
}

static void usage(const char *prog) {
    printf("Usage: %s [options] [test]\n", prog);
    printf("Options:\n");
    printf("  --log <file>    Log file path (default: timing_test.log)\n");
    printf("  --help, -h      Show this help\n");
    printf("\nTests:\n");
    printf("  demo            Run timing API demonstration\n");
    printf("  benchmark       Run comprehensive benchmarks\n");
    printf("  precision       Compare timing precision methods\n");
    printf("  all             Run all tests (default)\n");
}

int main(int argc, char *argv[]) {
    const char *log_file = "timing_test.log";
    const char *test_name = "all";
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i+1 < argc) {
            log_file = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            test_name = argv[i];
        }
    }
    
    // Initialize timing system
    if (timing_init(log_file) < 0) {
        fprintf(stderr, "Failed to initialize timing system\n");
        return 1;
    }
    
    timing_log(LOG_INFO, "=== Timing Test Suite Started ===");
    timing_log(LOG_INFO, "Log file: %s", log_file);
    
    printf("Timing Test Suite\n");
    printf("Log file: %s\n\n", log_file);
    
    // Run requested tests
    if (!strcmp(test_name, "demo") || !strcmp(test_name, "all")) {
        demonstrate_timing_api();
    }
    
    if (!strcmp(test_name, "benchmark") || !strcmp(test_name, "all")) {
        run_comprehensive_benchmarks();
    }
    
    if (!strcmp(test_name, "precision") || !strcmp(test_name, "all")) {
        test_precision_comparison();
    }
    
    timing_log(LOG_INFO, "=== Timing Test Suite Completed ===");
    
    printf("All tests completed. Check %s for detailed timing logs.\n", log_file);
    printf("Recent log entries:\n");
    printf("==================\n");
    
    // Show recent log entries
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tail -n 10 %s", log_file);
    system(cmd);
    
    timing_cleanup();
    return 0;
}
#endif /* TIMING_STANDALONE */
